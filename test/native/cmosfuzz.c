/* Fuzzes the CMOS values coreboot reads: newbios.c (compiled unchanged, included for its field tables)
   runs its romstage chipset options, its ramstage Auto timings and the ramstage PIRQ routing
   (pirq_routing.c, irq_tables.c, um8886 pirq.c) against CMOS, PCI and PIT seams. */
#define __SIMPLE_DEVICE__
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "newbios.c"
#include <device/pci.h>

unsigned long write_pirq_routing_table(unsigned long addr);

struct cmos_case {
	u8 settings[NB_COUNT];
	int byte255;		/* CMOS byte set to FFh (sum kept right), else -1 */
	int validity;		/* 0 valid, 1 wrong signature, 2 wrong sum */
	u8 bootfail;
	u8 cpu_sig;
	int cpuid;
	u16 pit;		/* PIT channel 2 count read back after the timed loop */
};

enum { TRAP = 1, FIELD_BITS = 2, FIELD_VALUE = 4, MANUAL_AUTO = 8, BOOTFAIL = 16, PIRQ_MASK = 32, SHUTDOWN = 64, KINDS = 7 };
static const char *const kind_names[KINDS] = {
	"trap (array bounds, division by zero or shift)", "register bits outside the documented fields written",
	"register field written with a value outside its domain", "romstage and ramstage disagree on manual/Auto",
	"boot failure counter not counted", "PCI IRQ outside the routing table's mask and the setup choices",
	"CMOS 0Fh (shutdown code) not cleared by a full POST"
};

struct machine {
	u8 cmos[128];
	u8 hb[256], isa[256];
	u8 port61;
	int pit_reads;
	const struct cmos_case *c;
	int writes;
	u8 irq_slot[CONFIG_IRQ_SLOT_COUNT][4];
	sigjmp_buf trap;
};
static __thread struct machine *mc;

static void fatal(const char *what, unsigned value)
{
	fprintf(stderr, "cmosfuzz: %s %x\n", what, value);
	exit(2);
}

int printk(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
	return 0;
}

u8 cmos_read(u8 addr)
{
	if (addr >= sizeof(mc->cmos))
		fatal("CMOS read at", addr);
	return mc->cmos[addr];
}

void cmos_write(u8 value, u8 addr)
{
	if (addr >= sizeof(mc->cmos))
		fatal("CMOS write at", addr);
	mc->cmos[addr] = value;
}

/* PIT channel 2: the count is read low byte first. */
u8 inb(u16 port)
{
	if (port == 0x61)
		return mc->port61;
	if (port != 0x42)
		fatal("inb from port", port);
	return mc->c->pit >> (8 * (mc->pit_reads++ & 1));
}

void outb(u8 value, u16 port)
{
	if (port == 0x61)
		mc->port61 = value;
	else if (port == 0x42 || port == 0x43)
		mc->pit_reads = 0;
	else
		fatal("outb to port", port);
}

int cpu_have_cpuid(void)
{
	return mc->c->cpuid;
}

u32 cpuid_eax(u32 op)
{
	if (op != 1)
		fatal("cpuid leaf", op);
	return mc->c->cpu_sig << 4;
}

static u8 *config_space(pci_devfn_t dev, u16 reg)
{
	if (reg > 0xff || (dev != HOST_BRIDGE && dev != ISA_BRIDGE))
		fatal("config access, bdf/reg", dev << 8 | reg);
	return (dev == HOST_BRIDGE ? mc->hb : mc->isa) + reg;
}

u8 pci_read_config8(pci_devfn_t dev, u16 reg)
{
	return *config_space(dev, reg);
}

void pci_write_config8(pci_devfn_t dev, u16 reg, u8 value)
{
	*config_space(dev, reg) = value;
	mc->writes++;
}

static struct device slots[32], isa_bridge = { ISA_BRIDGE };

struct device *pcidev_path_on_bus(unsigned int bus, pci_devfn_t devfn)
{
	if (bus || devfn >= 0x100)
		fatal("device path", bus << 8 | devfn);
	slots[devfn >> 3].bdf = PCI_DEV(0, devfn >> 3, devfn & 7);
	return &slots[devfn >> 3];
}

struct device *pcidev_on_root(u8 dev, u8 fn)
{
	if (PCI_DEV(0, dev, fn) != ISA_BRIDGE)
		fatal("root device", dev << 3 | fn);
	return &isa_bridge;
}

/* The board's PCI slots are devices 3-5, in the order of irq_tables.c. */
void pci_assign_irqs(struct device *dev, const unsigned char irqs[4])
{
	unsigned slot = ((dev->bdf >> 15) & 0x1f) - 3;

	if (slot >= CONFIG_IRQ_SLOT_COUNT)
		fatal("IRQs assigned to device", dev->bdf);
	memcpy(mc->irq_slot[slot], irqs, 4);
}

void pci_dev_write_config8(const struct device *dev, u16 reg, u8 value)
{
	pci_write_config8(dev->bdf, reg, value);
}

static void on_trap(int sig)
{
	(void)sig;
	siglongjmp(mc->trap, 1);
}

/* A register field and the values it may take. */
struct domain {
	pci_devfn_t dev;
	u8 reg, mask, count, value[4];
};

static int in_domain(const struct domain *d, u8 now, u8 before)
{
	if ((now & d->mask) == (before & d->mask))
		return 1;
	for (int i = 0; i < d->count; i++)
		if ((now & d->mask) == d->value[i])
			return 1;
	return 0;
}

/* Fields newbios.c writes besides its tables: Auto timings (rows of newbios_auto_timings), L2 off. */
static const struct domain extra_fields[] = {
	{ HOST_BRIDGE, 0x50, 0xc0, 1, { 0x00 } },
	{ HOST_BRIDGE, 0x58, 0xff, 1, { 0x0f } },
	{ HOST_BRIDGE, 0x59, 0xff, 1, { 0x00 } },
	{ ISA_BRIDGE, 0x56, 0x03, 4, { 0, 1, 2, 3 } },	/* ISA clock divider: no documented reserved value */
};

static void apply_case(const struct cmos_case *c)
{
	static const u8 hb_defaults[][2] = {	/* typical after romstage: 256K L2, 32 MB */
		{ 0x50, 0x93 }, { 0x51, 0xa4 }, { 0x52, 0x8d }, { 0x53, 0x81 }, { 0x55, 0x40 }, { 0x58, 0xff },
		{ 0x59, 0x0f }, { 0x5a, 0x0d }, { 0x5d, 0x30 }, { 0x61, 0x04 }, { 0x62, 0x01 },
	};
	static const u8 isa_defaults[][2] = {
		{ 0x40, 0x04 }, { 0x41, 0x42 }, { 0x46, 0x1f }, { 0x56, 0x01 }, { 0xa4, 0x01 },
	};
	u16 sum = 0;

	memset(mc->cmos, 0, sizeof(mc->cmos));
	for (int i = 0; i < NB_COUNT; i++)
		mc->cmos[NB_CMOS_BASE + i / 2] |= c->settings[i] << (i % 2 * 4);
	if (c->byte255 >= 0)
		mc->cmos[c->byte255] = 0xff;
	mc->cmos[NB_CMOS_SIG] = c->validity == 1 ? NB_SIG_VALUE ^ 1 : NB_SIG_VALUE;
	for (int i = NB_CMOS_BASE; i <= NB_CMOS_SIG; i++)
		sum += mc->cmos[i];
	sum += c->validity == 2;
	mc->cmos[NB_CMOS_SUM] = sum >> 8;
	mc->cmos[NB_CMOS_SUM + 1] = sum;
	mc->cmos[NB_CMOS_BOOTFAIL] = c->bootfail;
	mc->cmos[NB_CMOS_CPUSIG] = c->cpu_sig;
	memset(mc->hb, 0, sizeof(mc->hb));
	memset(mc->isa, 0, sizeof(mc->isa));
	for (unsigned i = 0; i < ARRAY_SIZE(hb_defaults); i++)
		mc->hb[hb_defaults[i][0]] = hb_defaults[i][1];
	for (unsigned i = 0; i < ARRAY_SIZE(isa_defaults); i++)
		mc->isa[isa_defaults[i][0]] = isa_defaults[i][1];
	mc->port61 = 0;
	mc->pit_reads = 0;
	mc->c = c;
}

static int field_failures(const u8 *hb0, const u8 *isa0)
{
	const struct nb_field *tables[] = { chipset, caching };
	const unsigned counts[] = { ARRAY_SIZE(chipset), ARRAY_SIZE(caching) };
	u8 allowed_hb[256] = { 0 }, allowed_isa[256] = { 0 };
	int fails = 0;

	for (int t = 0; t < 2; t++)
		for (unsigned i = 0; i < counts[t]; i++) {
			const struct nb_field *f = &tables[t][i];
			struct domain d = { f->ibc ? ISA_BRIDGE : HOST_BRIDGE, f->reg, f->mask, f->count, { 0 } };
			memcpy(d.value, f->value, sizeof(d.value));
			(f->ibc ? allowed_isa : allowed_hb)[f->reg] |= f->mask;
			const u8 *now = f->ibc ? mc->isa : mc->hb;
			const u8 *before = f->ibc ? isa0 : hb0;
			if (!in_domain(&d, now[f->reg], before[f->reg]))
				fails |= FIELD_VALUE;
		}
	for (unsigned i = 0; i < ARRAY_SIZE(extra_fields); i++) {
		const struct domain *d = &extra_fields[i];
		int hb = d->dev == HOST_BRIDGE;
		(hb ? allowed_hb : allowed_isa)[d->reg] |= d->mask;
		if (!in_domain(d, (hb ? mc->hb : mc->isa)[d->reg], (hb ? hb0 : isa0)[d->reg]))
			fails |= FIELD_VALUE;
	}
	allowed_isa[IBC43_INTAB_IRQ] = allowed_isa[IBC44_INTCD_IRQ] = 0xff;	/* checked as IRQs */
	for (int reg = 0; reg < 256; reg++)
		if (((mc->hb[reg] ^ hb0[reg]) & ~allowed_hb[reg]) || ((mc->isa[reg] ^ isa0[reg]) & ~allowed_isa[reg]))
			fails |= FIELD_BITS;
	return fails;
}

/* auto routing stays in the table's bitmap; setup may also choose 3, 4, 5, 7, 9-12, 14, 15 */
#define SETUP_PCI_IRQS 0xdeb8

static int pirq_failures(const struct irq_routing_table *rt)
{
	u16 link_mask[CONFIG_MAX_PIRQ_LINKS + 1] = { 0 };
	int fails = 0;

	for (int s = 0; s < CONFIG_IRQ_SLOT_COUNT; s++)
		for (int intx = 0; intx < 4; intx++) {
			u8 irq = mc->irq_slot[s][intx], link = rt->slots[s].irq[intx].link;
			u16 bitmap = rt->slots[s].irq[intx].bitmap;
			if (link <= CONFIG_MAX_PIRQ_LINKS)
				link_mask[link] |= bitmap;
			if (irq && (irq > 15 || !((bitmap | SETUP_PCI_IRQS) >> irq & 1)))
				fails |= PIRQ_MASK;
		}
	const u8 route[4] = { mc->isa[IBC43_INTAB_IRQ] >> 4, mc->isa[IBC43_INTAB_IRQ] & 15,
			      mc->isa[IBC44_INTCD_IRQ] >> 4, mc->isa[IBC44_INTCD_IRQ] & 15 };
	for (int link = 0; link < 4; link++)
		if (route[link] && !((link_mask[link + 1] | SETUP_PCI_IRQS) >> route[link] & 1))
			fails |= PIRQ_MASK;
	return fails;
}

static int run_case(const struct cmos_case *c)
{
	static __thread unsigned long table[(32 + 16 * CONFIG_IRQ_SLOT_COUNT) / sizeof(long) + 4];
	u8 hb0[256], isa0[256];
	apply_case(c);
	volatile int manual = !c->validity && !NB_CMOS_GET(cmos_read, NB_CHIPSET_AUTO) && c->bootfail < NB_BOOTFAIL_MAX;
	memcpy(hb0, mc->hb, sizeof(hb0));
	memcpy(isa0, mc->isa, sizeof(isa0));
	memset(mc->irq_slot, 0, sizeof(mc->irq_slot));
	if (sigsetjmp(mc->trap, 1))
		return TRAP;
	mc->cmos[0x0f] = c->bootfail ^ 0x5a;
	newbios_chipset_options();
	int fails = mc->cmos[NB_CMOS_BOOTFAIL] != (c->bootfail < 0xff ? c->bootfail + 1 : 0xff) ? BOOTFAIL : 0;
	if (mc->cmos[0x0f])
		fails |= SHUTDOWN;
	mc->writes = 0;
	newbios_auto_timings();
	if (!manual != !!mc->writes)
		fails |= MANUAL_AUTO;
	unsigned long addr = ALIGN_UP((unsigned long)table, 16);
	write_pirq_routing_table(addr);
	return fails | field_failures(hb0, isa0) | pirq_failures((const struct irq_routing_table *)addr);
}

static const u8 cpu_sigs[] = { 0x00, 0xff, 0x40, 0x41, 0x43, 0x45, 0x47, 0x48, 0x49, 0x4e, 0x4f, 0x35, 0x53, 0x5f, 0x3f };
/* Byte-symmetric: cpu_mhz's two inb(0x42) are unsequenced, the host compiler may read them in either
   order. 15 MHz, 0 ticks, 3.7 GHz, 265 MHz, 133 MHz, 29 MHz. */
static const u16 pit_counts[] = { 0x0000, 0xffff, 0xfefe, 0xf1f1, 0xe3e3, 0x8080 };
static const u8 bootfails[] = { 0, NB_BOOTFAIL_MAX - 1, NB_BOOTFAIL_MAX, NB_BOOTFAIL_MAX + 1, 254, 255 };

static struct cmos_case *cases;
static int case_count, case_max;

static struct cmos_case *new_case(void)
{
	if (case_count == case_max && !(cases = realloc(cases, (case_max = 2 * case_max + 1024) * sizeof(*cases))))
		fatal("out of memory, cases", case_max);
	struct cmos_case *c = &cases[case_count++];
	memset(c, 0, sizeof(*c));
	c->byte255 = -1;
	c->cpu_sig = 0x4f;		/* Am5x86-133: CPUID, x4, 33 MHz bus */
	c->cpuid = 1;
	c->pit = 0xe3e3;
	return c;
}

/* Smallest cases first, so the first failing case of a kind is a minimal one. */
static void build_cases(void)
{
	for (int auto_on = 0; auto_on < 2; auto_on++)
		for (int validity = 0; validity < 3; validity++)
			for (unsigned b = 0; b < ARRAY_SIZE(bootfails); b++)
				for (unsigned s = 0; s < ARRAY_SIZE(cpu_sigs); s++)
					for (int cpuid = 0; cpuid < 2; cpuid++)
						for (unsigned p = 0; p < ARRAY_SIZE(pit_counts); p++) {
							struct cmos_case *c = new_case();
							c->settings[NB_CHIPSET_AUTO] = auto_on;
							c->validity = validity;
							c->bootfail = bootfails[b];
							c->cpu_sig = cpu_sigs[s];
							c->cpuid = cpuid;
							c->pit = pit_counts[p];
						}
	for (int base = 0; base < 4; base++)
		for (unsigned b = 0; b < ARRAY_SIZE(bootfails); b++) {
			for (int setting = 0; setting < NB_COUNT; setting++)
				for (int value = 0; value < 16; value++) {
					struct cmos_case *c = new_case();
					c->settings[NB_CHIPSET_AUTO] = base & 1;
					c->settings[NB_L2_CACHE] = base >> 1;
					c->settings[setting] = value;
					c->bootfail = bootfails[b];
				}
			for (int byte = NB_CMOS_BASE; byte <= NB_CMOS_BASE + (NB_COUNT - 1) / 2; byte++) {
				struct cmos_case *c = new_case();
				c->settings[NB_CHIPSET_AUTO] = base & 1;
				c->settings[NB_L2_CACHE] = base >> 1;
				c->byte255 = byte;
				c->bootfail = bootfails[b];
			}
		}
	uint64_t x = 0x2545f4914f6cdd1dULL;
	for (int n = 0; n < 20000; n++) {
		struct cmos_case *c = new_case();
		for (int i = 0; i < NB_COUNT; i++) {
			x ^= x << 13, x ^= x >> 7, x ^= x << 17;
			c->settings[i] = x & 15;
		}
		c->validity = (x >> 8) % 8 == 0 ? 1 + (x >> 11) % 2 : 0;
		c->bootfail = bootfails[(x >> 16) % ARRAY_SIZE(bootfails)];
		c->cpu_sig = cpu_sigs[(x >> 24) % ARRAY_SIZE(cpu_sigs)];
		c->cpuid = (x >> 32) & 1;
		c->pit = pit_counts[(x >> 40) % ARRAY_SIZE(pit_counts)];
	}
}

static int *results;
static int next_case;

static void *worker(void *unused)
{
	(void)unused;
	if (!(mc = calloc(1, sizeof(*mc))))
		fatal("out of memory, bytes", sizeof(*mc));
	for (int i; (i = __atomic_fetch_add(&next_case, 1, __ATOMIC_RELAXED)) < case_count;)
		results[i] = run_case(&cases[i]);
	return NULL;
}

static const char *const setting_names[NB_COUNT] = {
	[NB_L2_CACHE] = "L2_CACHE", [NB_CHIPSET_AUTO] = "CHIPSET_AUTO", [NB_DRAM_READ_WS] = "DRAM_READ_WS",
	[NB_DRAM_WRITE_WS] = "DRAM_WRITE_WS", [NB_L2_WS] = "L2_WS", [NB_KBC_CLOCK] = "KBC_CLOCK", [NB_EDO] = "EDO",
	[NB_EARLY_CACHE_WRITE] = "EARLY_CACHE_WRITE", [NB_SYSBIOS_CACHE] = "SYSBIOS_CACHE",
	[NB_VIDBIOS_CACHE] = "VIDBIOS_CACHE", [NB_HOST_PCI_POST_WS] = "HOST_PCI_POST_WS",
	[NB_HOST_PCI_BURST] = "HOST_PCI_BURST", [NB_PCI_BUS_PARK] = "PCI_BUS_PARK",
	[NB_PCI_POSTED_WRITE] = "PCI_POSTED_WRITE", [NB_SLOW_REFRESH] = "SLOW_REFRESH",
	[NB_HOST_PCI_CLOCK] = "HOST_PCI_CLOCK", [NB_BURST_COPYBACK] = "BURST_COPYBACK",
	[NB_PREEMPT_PCI] = "PREEMPT_PCI", [NB_DEVSEL] = "DEVSEL", [NB_PCI_IRQ_A] = "PCI_IRQ_A",
	[NB_PCI_IRQ_B] = "PCI_IRQ_B", [NB_PCI_IRQ_C] = "PCI_IRQ_C", [NB_PCI_IRQ_D] = "PCI_IRQ_D",
};

static void print_case(const struct cmos_case *c)
{
	static const char *const validity[] = { "valid", "wrong signature", "wrong sum" };

	printf("CMOS %s", validity[c->validity]);
	for (int i = 0; i < NB_COUNT; i++)
		if (c->settings[i]) {
			if (setting_names[i])
				printf(", NB_%s=%d", setting_names[i], c->settings[i]);
			else
				printf(", setting %d=%d", i, c->settings[i]);
		}
	if (c->byte255 >= 0)
		printf(", byte %02Xh=FFh", c->byte255);
	printf(", BOOTFAIL=%d, CPUSIG=%02Xh%s, PIT count %04Xh", c->bootfail, c->cpu_sig,
	       c->cpuid ? " (CPUID)" : " (no CPUID)", c->pit);
}

int main(void)
{
	struct sigaction trap = { .sa_handler = on_trap };
	struct timespec t0, t1;
	pthread_t threads[256];
	long workers = sysconf(_SC_NPROCESSORS_ONLN);

	if (sigaction(SIGILL, &trap, NULL) || sigaction(SIGFPE, &trap, NULL)) {
		perror("sigaction");
		return 2;
	}
	if (workers < 1 || workers > 256)
		fatal("CPUs", workers);
	build_cases();
	if (!(results = calloc(case_count, sizeof(*results))))
		fatal("out of memory, cases", case_count);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long i = 0; i < workers; i++)
		if (pthread_create(&threads[i], NULL, worker, NULL)) {
			perror("pthread_create");
			return 2;
		}
	for (long i = 0; i < workers; i++)
		if (pthread_join(threads[i], NULL)) {
			perror("pthread_join");
			return 2;
		}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("%d cases in %.2f s: %.0f cases/s, %ld workers\n", case_count, seconds, case_count / seconds, workers);

	int failing = 0;
	for (int kind = 0; kind < KINDS; kind++) {
		int count = 0, first = -1;
		for (int i = 0; i < case_count; i++)
			if (results[i] & (1 << kind) && !count++)
				first = i;
		failing += count;
		if (!count)
			continue;
		printf("FAIL %d cases: %s; first: ", count, kind_names[kind]);
		print_case(&cases[first]);
		printf("\n");
	}
	return failing ? 1 : 0;
}
