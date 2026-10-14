/* Runs coreboot_486's UM8881 dram_init + l2_cache_init, then SeaBIOS's setup_l2_cache, against the model
   (umc_hb4.c port plus normal-mode L2) for every SIMM combination and L2 hardware variant: checks the
   sizing, that the L2 ends on only when it works, that reads with it on return what was written, and
   compares with the oracle lines read on stdin. Workers are processes: setup.c keeps plain globals. */
#define __SIMPLE_DEVICE__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "newbios_cmos.h"
#include "reg_init.h"
#include "um8881_model.h"

void dram_init(void);
void l2_cache_init(void);
void romstage_chipset_defaults(void);
void setup_l2_cache(void);
void seabios_l2_settings(int l2_cache, int defaulted);

static __thread struct model *m;
static __thread int printk_errors;
__thread uintptr_t seam_next_read = 1;
static __thread uintptr_t queue_start = 1;

static void fatal(const char *what, unsigned value)
{
	fprintf(stderr, "dramfuzz: %s %x\n", what, value);
	exit(2);
}

static void flush_reads(void)
{
	if (queue_start != seam_next_read)
		model_read_run(m, queue_start, seam_next_read);
	queue_start = seam_next_read;
}

int printk(int level, const char *fmt, ...)
{
	if (level <= BIOS_ERR)
		printk_errors++;
	(void)fmt;
	return 0;
}

/* coreboot seams */
static uint8_t *config_space(pci_devfn_t dev, u16 reg)
{
	if (reg > 0xff || (dev != HOST_BRIDGE && dev != ISA_BRIDGE))
		fatal("config access not modelled, bdf/reg", dev << 8 | reg);
	return (dev == HOST_BRIDGE ? m->hb : m->isa) + reg;
}

u8 pci_read_config8(pci_devfn_t dev, u16 reg)
{
	return *config_space(dev, reg);
}

u16 pci_read_config16(pci_devfn_t dev, u16 reg)
{
	return pci_read_config8(dev, reg) | pci_read_config8(dev, reg + 1) << 8;
}

void pci_write_config8(pci_devfn_t dev, u16 reg, u8 value)
{
	uint8_t *byte = config_space(dev, reg);

	flush_reads();
	if (dev == HOST_BRIDGE)
		model_hb_write(m, reg, value);
	else
		*byte = value;
}

void pci_write_config16(pci_devfn_t dev, u16 reg, u16 value)
{
	pci_write_config8(dev, reg, value);
	pci_write_config8(dev, reg + 1, value >> 8);
}

u32 seam_read32(uintptr_t addr)
{
	flush_reads();
	queue_start = seam_next_read = addr + 4;
	return model_read32(m, addr);
}

u32 seam_read32_queued(uintptr_t addr)
{
	seam_next_read = addr;
	flush_reads();
	queue_start = seam_next_read = addr + 4;
	return model_read32(m, addr);
}

void write32p(uintptr_t addr, u32 value)
{
	flush_reads();
	model_write32(m, addr, value);
}

static u8 cmos[128];

u8 cmos_read(u8 addr)
{
	if (addr >= sizeof(cmos))
		fatal("CMOS read at", addr);
	return cmos[addr];
}

void cmos_write(u8 value, u8 addr)
{
	if (addr >= sizeof(cmos))
		fatal("CMOS write at", addr);
	cmos[addr] = value;
}

/* SeaBIOS seams (setup_l2_cache): host bridge 00:10.0, CMOS, CR0, RAM size, memory */
#define SEABIOS_CR0 0x00000011	/* PE, ET: 32-bit flat, caches on (transition32 clears CD) */
#define CR0_CD_NW 0x60000000
u32 LegacyRamSize;
static u32 cr0;
static int cr0_cd_seen, l2_fills, l2_fill_cached_off;

u8 pci_config_readb(u16 bdf, u32 reg)
{
	if (bdf != 0x80 || reg > 0xff)
		fatal("SeaBIOS config read not modelled, bdf/reg", bdf << 8 | reg);
	return m->hb[reg];
}

void pci_config_writeb(u16 bdf, u32 reg, u8 value)
{
	if (bdf != 0x80 || reg > 0xff)
		fatal("SeaBIOS config write not modelled, bdf/reg", bdf << 8 | reg);
	model_hb_write(m, reg, value);
}

u8 rtc_read(u8 index)
{
	if (index != NB_CMOS_BOOTFAIL && index != NB_CMOS_L2 && index != NB_CMOS_LASTPOST)
		fatal("SeaBIOS CMOS read not modelled at", index);
	return cmos[index];
}

void rtc_write(u8 index, u8 val)
{
	if (index != NB_CMOS_L2 && index != NB_CMOS_LASTPOST)
		fatal("SeaBIOS CMOS write not modelled at", index);
	cmos[index] = val;
}

void outb(u8 value __attribute__((unused)), u16 port)   /* the L2 steps' POST codes, also on COM1 */
{
	if (port != 0x80 && (port < 0x3f8 || port > 0x3fc))
		fatal("SeaBIOS port write not modelled at", port);
}

u8 inb(u16 port)                                        /* COM1 line status: always ready to send */
{
	if (port != 0x3fd)
		fatal("SeaBIOS port read not modelled at", port);
	return 0x60;
}

u32 cr0_read(void)
{
	return cr0;
}

void cr0_write(u32 value)
{
	cr0 = value;
	cr0_cd_seen |= (value & CR0_CD_NW) == CR0_CD_NW;
}

void wbinvd(void)
{
}

void msleep(u32 ms __attribute__((unused)))
{
}

u32 getesp(void)
{
	return 0x7000;
}

u32 readl(const void *addr)
{
	return model_read32(m, (uintptr_t)addr);
}

/* setup_l2_cache's test dwords and what was there before it (e.g. the IVT at 0-3FFh) */
static struct { u32 addr, original; } touched[16];
static int touched_count;

void writel(void *addr, u32 value)
{
	int i;

	for (i = 0; i < touched_count && touched[i].addr != (uintptr_t)addr; i++)
		;
	if (i == touched_count) {
		if (touched_count == ARRAY_SIZE(touched))
			fatal("SeaBIOS writes more dwords than recorded, at", (uintptr_t)addr);
		touched[touched_count++] = (typeof(touched[0])){ (uintptr_t)addr, model_dram_peek(m, (uintptr_t)addr) };
	}
	model_write32(m, (uintptr_t)addr, value);
}

/* setup.c's asm (weakened in its object): 50h, then rep lodsl over the range. */
void l2_fill(u32 base, u32 dwords, u8 hb50)
{
	l2_fills++;
	l2_fill_cached_off |= (cr0 & CR0_CD_NW) != CR0_CD_NW || (m->hb[0x55] & 0x21);
	model_hb_write(m, 0x50, hb50);
	model_read_run(m, base, base + 4 * dwords);
}

/* setup.c's stackless asm checks (weakened in its object), the same steps on the model. */
u32 l2_line_works(u32 addr, u32 size, u8 hb50)
{
	u32 value = 0x5aa5c33c ^ addr, saved;
	model_hb_write(m, 0x50, hb50 & ~0x80);
	saved = model_read32(m, addr);
	model_write32(m, addr, value);
	model_hb_write(m, 0x50, hb50);
	model_read32(m, addr ^ size);
	u32 works = model_read32(m, addr) == value && model_read32(m, addr) == value;
	model_hb_write(m, 0x50, hb50 & ~0x80);
	model_write32(m, addr, saved);
	model_hb_write(m, 0x50, hb50);
	model_read32(m, addr ^ size);
	return works;
}

u32 l2_caches(u32 addr, u32 size, u8 hb50)
{
	u32 saved = model_read32(m, addr);
	model_read32(m, addr);
	model_hb_write(m, 0x50, hb50 & ~0x80);
	model_write32(m, addr, ~saved);
	model_hb_write(m, 0x50, hb50);
	u32 cached = model_read32(m, addr) == saved;
	model_hb_write(m, 0x50, hb50 & ~0x80);
	model_write32(m, addr, saved);
	model_hb_write(m, 0x50, hb50);
	model_read32(m, addr ^ size);
	return cached;
}

enum {
	TOP_LOW = 1, TOP_HIGH = 2, HOLE = 4, ALIAS = 8, TOP128K = 16, FIELDS = 32, L2 = 64, IBC51 = 128,
	L2_WRONG = 256, L2_ON = 512, SEABIOS = 1024, CLOBBER = 2048, FAIL_KINDS = 12
};
static const char *const kind_names[FAIL_KINDS] = {
	"top below what the SIMMs give", "top above what the SIMMs give", "hole (absent cell) below top",
	"alias below top", "bad page in top-128K..top (postcar)", "register bits outside the sizing fields",
	"L2 left by coreboot (50h 0, size/banks in CMOS 3Eh)", "IBC 51h memory size",
	"read with L2 on differs from what was written", "L2 on/off after SeaBIOS (50h bits 7-6)",
	"SeaBIOS left CR0 or other host bridge registers changed, or filled with caches on",
	"SeaBIOS left its test dwords in memory (e.g. the INT 40h vector at 100h)"
};

enum { ABSENT, REAL, RANDOM, FAKE_DATA_FF, FAKE_DATA_BUS, FAKE_TAGS, BOTH_FAKE, GUARD, CLASSES };
static const char *const class_names[CLASSES] = {
	"absent", "real", "real, random power-on", "fake data (floats FFh)", "fake data (floats bus)",
	"fake tags", "fake data and tags", "real 256K, setup/CMOS guard"
};
#define SEEDS 3
#define GUARDS (2 * 2 * 5 - 1)
#define VARIANTS (1 + 5 * 2 * (BOTH_FAKE - 1 + SEEDS) + GUARDS)

struct variant {
	int class;
	struct l2_hw hw;
	int l2_setting, defaulted;	/* setup's External Cache, CMOS invalid (SetupDefaulted) */
	u8 bootfail;			/* CMOS POST counter as SeaBIOS reads it (coreboot counted this POST) */
};
static struct variant variants[VARIANTS];

static void build_variants(void)
{
	static const u8 bootfails[] = { 0, 1, NB_BOOTFAIL_MAX, NB_BOOTFAIL_MAX + 1, 255 };
	int n = 0;

	variants[n++] = (struct variant){ ABSENT, { 0, 1, L2_DATA_FLOAT_FF, 1, 0 }, 1, 0, 1 };
	for (unsigned kb = 64; kb <= 1024; kb *= 2)
		for (int banks = 1; banks <= 2; banks++) {
			variants[n++] = (struct variant){ REAL, { kb, banks, L2_DATA_REAL, 0, 0 }, 1, 0, 1 };
			for (unsigned seed = 1; seed <= SEEDS; seed++)
				variants[n++] = (struct variant){ RANDOM, { kb, banks, L2_DATA_REAL, 0, seed }, 1, 0, 1 };
			variants[n++] = (struct variant){ FAKE_DATA_FF, { kb, banks, L2_DATA_FLOAT_FF, 0, 0 }, 1, 0, 1 };
			variants[n++] = (struct variant){ FAKE_DATA_BUS, { kb, banks, L2_DATA_FLOAT_BUS, 0, 0 }, 1, 0, 1 };
			variants[n++] = (struct variant){ FAKE_TAGS, { kb, banks, L2_DATA_REAL, 1, 0 }, 1, 0, 1 };
			variants[n++] = (struct variant){ BOTH_FAKE, { kb, banks, L2_DATA_FLOAT_BUS, 1, 0 }, 1, 0, 1 };
		}
	for (int l2_setting = 0; l2_setting < 2; l2_setting++)
		for (int defaulted = 0; defaulted < 2; defaulted++)
			for (unsigned b = 0; b < ARRAY_SIZE(bootfails); b++)
				if (!l2_setting || defaulted || bootfails[b] != 1)	/* that one is class REAL */
					variants[n++] = (struct variant){ GUARD, { 256, 1, L2_DATA_REAL, 0, 0 }, l2_setting,
									  defaulted, bootfails[b] };
	if (n != VARIANTS)
		fatal("variants built, expected", n << 16 | VARIANTS);
}

#define COMBOS 10000

struct result {
	uint32_t top;
	uint32_t expected;
	uint32_t bad_addr;
	uint32_t digest;	/* L2 state after setup_l2_cache, cross-checked combinations only */
	uint8_t r50_coreboot, r50, r52, r53, r61;
	uint16_t fails;
};

static uint32_t best_pair[SIMM_TYPES][SIMM_TYPES];

/* Largest one-to-one region the chipset can make of a SIMM pair: any size, banks, sides, swap and MA11. */
static void compute_best_pairs(void)
{
	const struct l2_hw none = { 0, 1, L2_DATA_FLOAT_FF, 1, 0 };

	for (int lo = 0; lo < SIMM_TYPES; lo++)
		for (int hi = 0; hi < SIMM_TYPES; hi++) {
			model_reset(m, (const int[4]){ lo, hi, 0, 0 }, &none);
			for (int code = 1; code < 8; code++)
				for (int cfg = 0; cfg < 8; cfg++)
					for (int ma11 = 0; ma11 < 2; ma11++) {
						const uint8_t regs[3] = { code, (cfg & 3) | (cfg & 4) << 2, ma11 << 4 };
						if (model_region_flat(m, regs, 0) && (1u << (20 + code)) > best_pair[lo][hi])
							best_pair[lo][hi] = 1u << (20 + code);
					}
		}
}

static uint8_t l2_code(unsigned kb)
{
	uint8_t code = 0;

	while (kb >= 64) {
		kb >>= 1;
		code++;
	}
	return code;
}

/* A page's cells are its base cell plus address bits 0-11 (column MA0-8, row MA0), which no other
   address bit drives: pages are present and distinct when their base cells are. */
static int check_pages(const uint8_t regs[3], uint32_t top, uint32_t *bad_addr)
{
	static uint8_t *seen;
	int fails = 0;

	if (!seen && !(seen = malloc(512 << 20 >> 14)))
		fatal("out of memory, bytes", 512 << 20 >> 14);
	memset(seen, 0, m->capacity >> 14);
	for (uint32_t addr = 0; addr < top; addr += 0x1000) {
		if (addr == 0xa0000)
			addr = 0x100000;
		int64_t cell = model_dram_decode(m, regs, addr);
		int fail = cell < 0 ? HOLE : (seen[cell >> 14] >> ((cell >> 11) & 7)) & 1 ? ALIAS : 0;
		if (fail) {
			if (!fails)
				*bad_addr = addr;
			fails |= fail | (addr >= top - 0x20000 ? TOP128K : 0);
		} else
			seen[cell >> 14] |= 1 << ((cell >> 11) & 7);
	}
	return fails;
}

/* coreboot sizes the L2, leaves 50h 0 and hands the data SRAM's size and banks to SeaBIOS in CMOS. */
static int check_coreboot(const uint8_t *before, uint8_t ibc51_before, uint32_t top, const struct variant *v)
{
	const uint8_t *hb = m->hb;
	int code_l = hb[0x52] & 7, code_u = (hb[0x52] >> 4) & 7;
	uint8_t region_bits53 = (code_l ? 0x13 : 0) | (code_u ? 0x2c : 0);
	uint8_t region_bits61 = (code_l ? 0x10 : 0) | (code_u ? 0x20 : 0);
	int fails = 0;

	if (((hb[0x52] ^ before[0x52]) & 0x80) || ((hb[0x53] ^ before[0x53]) & 0xc0) ||
	    ((hb[0x61] ^ before[0x61]) & 0xcf) || hb[0x5a] != (before[0x5a] & ~0x40) ||
	    (code_u && !code_l) || code_u > code_l || (hb[0x53] & 0x3f & ~region_bits53) ||
	    (hb[0x61] & 0x30 & ~region_bits61))
		fails |= FIELDS;
	for (int reg = 0; reg < 256; reg++)
		if (reg != 0x50 && reg != 0x52 && reg != 0x53 && reg != 0x5a && reg != 0x61 && hb[reg] != before[reg])
			fails |= FIELDS;
	uint8_t sized = (v->hw.banks == 2 ? 8 : 0) | l2_code(v->hw.kb);
	if (hb[0x50] || (cmos[NB_CMOS_L2] & 0x0f) != (v->hw.kb && v->hw.data == L2_DATA_REAL ? sized : 0)
	    || ((cmos[NB_CMOS_L2] & 0x70) && !(cmos[NB_CMOS_L2] & 0x80)))	/* a step only with the in-progress bit */
		fails |= L2;
	uint32_t size_4m = top / (4 << 20) ? top / (4 << 20) : 1;
	if (m->isa[0x51] != (((size_4m & 0x7f) << 1) | (ibc51_before & 1)))
		fails |= IBC51;
	return fails;
}

static int read_back(uint32_t addr, uint32_t want, uint32_t *bad_addr)
{
	if (model_read32(m, addr) == want)
		return 0;
	*bad_addr = addr;
	return L2_WRONG;
}

/* With the L2 caching (all lines clean when SeaBIOS turns write-back on): every line's current hit
   against DRAM (initial false hits), then writes and reads of conflicting tags (0, 1, mixed, all ones
   below the cacheable end) on the first, middle, last and aliased lines, against the values written. */
static int check_l2(uint32_t *bad_addr)
{
	uint32_t lines = model_l2_lines(m), end = model_l2_cacheable_end(m);
	int fails = 0;

	if (!lines || !end)
		return 0;
	for (uint32_t line = 0; line < lines; line++) {
		uint32_t addr = model_l2_false_hit(m, line);
		if (addr != NO_HIT) {
			*bad_addr = addr;
			return L2_WRONG;
		}
	}

	uint32_t size = lines << 4, tag_max = (end - 1) / size, installed = m->l2_size;
	const uint32_t tags[] = { 0, 1, tag_max & 0x55, tag_max };
	const uint32_t offsets[] = { 0, 16, size / 2, size - 16, installed < size ? installed : 0 };
	uint32_t addrs[ARRAY_SIZE(tags) * ARRAY_SIZE(offsets) * 4];
	unsigned n = 0;
	for (unsigned t = 0; t < ARRAY_SIZE(tags); t++)
		for (unsigned o = 0; o < ARRAY_SIZE(offsets); o++)
			for (uint32_t dword = 0; dword < 4; dword++) {
				uint32_t addr = tags[t] * size + offsets[o] + 4 * dword;
				int dup = 0;
				for (unsigned i = 0; i < n; i++)
					dup |= addrs[i] == addr;
				if (!dup && tags[t] <= tag_max && addr < end && (addr < 0xa0000 || addr >= 0x100000))
					addrs[n++] = addr;
			}
	for (uint32_t round = 0; round < 2 && !fails; round++) {
		for (unsigned i = 0; i < n; i++)
			model_write32(m, addrs[i], addrs[i] * 0x9e3779b1u + round);
		for (unsigned i = 0; i < n && !fails; i++)
			fails |= read_back(addrs[i], addrs[i] * 0x9e3779b1u + round, bad_addr);
		for (unsigned i = n; i-- > 0 && !fails;)
			fails |= read_back(addrs[i], addrs[i] * 0x9e3779b1u + round, bad_addr);
	}
	return fails;
}

/* SeaBIOS at POST: setup_l2_cache on what coreboot left; LegacyRamSize is the DRAM top less the reserved
   CBMEM (224 KiB: the 86Box e820 on 32 MB). */
static int run_seabios(const struct variant *v, uint8_t r50_coreboot)
{
	uint8_t before[256];
	int works = v->hw.kb && v->hw.data == L2_DATA_REAL && !v->hw.fake_tags;
	int hung = v->bootfail >= NB_BOOTFAIL_MAX;	/* the guard variants: an L2 init left unfinished */
	int allowed = v->l2_setting && (v->defaulted || !hung);
	int fails = 0;

	LegacyRamSize = m->top - 0x38000 >= 0x100000 ? m->top - 0x38000 : 0x100000;
	cmos[NB_CMOS_BOOTFAIL] = v->bootfail;
	cmos[NB_CMOS_L2] = (cmos[NB_CMOS_L2] & 0x7f) | (hung ? 0x80 : 0);
	cr0 = SEABIOS_CR0;
	cr0_cd_seen = l2_fills = l2_fill_cached_off = touched_count = 0;
	seabios_l2_settings(v->l2_setting, v->defaulted);
	memcpy(before, m->hb, sizeof(before));
	setup_l2_cache();
	if (cr0_cd_seen && !l2_fills)
		fatal("setup.c's own l2_fill ran (inlined?), 50h", m->hb[0x50]);
	for (int i = 0; i < touched_count; i++)
		if (model_read32(m, touched[i].addr) != touched[i].original)
			fails |= CLOBBER;
	if (cr0 != SEABIOS_CR0 || l2_fill_cached_off || memcmp(before, m->hb, 0x50) ||
	    memcmp(before + 0x51, m->hb + 0x51, sizeof(before) - 0x51))
		fails |= SEABIOS;
	if (cmos[NB_CMOS_L2] & 0x80)		/* the in-progress flag must be cleared */
		fails |= SEABIOS;
	/* on: write-back; failing its checks: fully off (size 0); size 0 or not allowed: untouched */
	uint8_t sized = (r50_coreboot & 0x30) | (cmos[NB_CMOS_L2] & 0x0f);
	uint8_t want = !(cmos[NB_CMOS_L2] & 7) || !allowed ? r50_coreboot : works ? sized | 0xc0 : sized & 0x30;
	/* fake tags whose all-ones tag lies above the DRAM top never hit falsely: on is harmless there */
	if (m->hb[0x50] != want && !(v->hw.fake_tags && want == (sized & 0x30) && m->hb[0x50] == (sized | 0xc0)))
		fails |= L2_ON;
	return fails;
}

struct page_memo {
	uint8_t regs[3];
	int valid, fails;
	uint32_t bad_addr;
};

enum { FAST, FAST_DIGEST, EXACT_DIGEST };

static void run_case(const int types[4], const struct variant *v, struct result *r, struct page_memo *memo,
		     int mode)
{
	uint8_t before[256];

	model_reset(m, types, &v->hw);
	cmos[NB_CMOS_L2] = 0xff;	/* stale from an earlier boot: coreboot must write it */
	m->exact_runs = mode == EXACT_DIGEST;
	seam_next_read = queue_start = 1;
	m->car_base = CONFIG_DCACHE_RAM_BASE;
	m->car_size = CONFIG_DCACHE_RAM_SIZE;
	/* cache_as_ram.S: temporary DRAM for the cache-as-RAM fill, then none */
	pci_write_config8(HOST_BRIDGE, 0x52, 0x77);
	pci_write_config8(HOST_BRIDGE, 0x53, 0x0f);
	pci_write_config8(HOST_BRIDGE, 0x52, 0x00);
	pci_write_config8(HOST_BRIDGE, 0x53, 0x00);
	romstage_chipset_defaults();
	memcpy(before, m->hb, sizeof(before));
	uint8_t ibc51_before = m->isa[0x51];
	printk_errors = 0;

	dram_init();
	l2_cache_init();
	flush_reads();
	m->car_size = 0;	/* postcar leaves cache-as-RAM */

	const uint8_t regs[3] = { m->hb[0x52], m->hb[0x53], m->hb[0x61] };
	r->top = cbmem_top_chipset();
	r->expected = best_pair[types[0]][types[1]] + best_pair[types[2]][types[3]];
	r->r50_coreboot = m->hb[0x50];
	r->r52 = regs[0];
	r->r53 = regs[1];
	r->r61 = regs[2];
	r->fails = (r->top < r->expected ? TOP_LOW : 0) | (r->top > r->expected ? TOP_HIGH : 0);
	if (r->top != model_region_size(regs, 0) + model_region_size(regs, 1) || printk_errors)
		r->fails |= FIELDS;
	if (!memo->valid || memcmp(memo->regs, regs, 3)) {
		memo->fails = check_pages(regs, r->top, &memo->bad_addr);
		memcpy(memo->regs, regs, 3);
		memo->valid = 1;
	}
	r->fails |= memo->fails;
	r->bad_addr = memo->bad_addr;
	r->fails |= check_coreboot(before, ibc51_before, r->top, v);
	if (r->top)	/* no DRAM: nothing boots further */
		r->fails |= run_seabios(v, r->r50_coreboot);
	r->r50 = m->hb[0x50];
	r->digest = mode == FAST ? 0 : model_digest(m);
	if (!(r->fails & ~(L2_ON | SEABIOS | CLOBBER)))	/* DRAM sized right: reads must be right whatever the L2 */
		r->fails |= check_l2(&r->bad_addr);
}

static void combo_types(int combo, int types[4])
{
	for (int bank = 0; bank < 4; bank++, combo /= 10)
		types[bank] = combo % 10;
}

static int combo_of(const int types[4])
{
	return types[0] + 10 * types[1] + 100 * types[2] + 1000 * types[3];
}

/* Shared with the worker processes. */
struct shared {
	int next_combo;
	unsigned cross_checked, cross_mismatches;
	int combo_order[COMBOS];	/* the slow cross-checked ones first: no straggler at the end */
	struct result results[COMBOS][VARIANTS];
};
static struct shared *sh;

/* The board's and every 625th combination also run with each queued read applied one by one. */
static int cross_check(int combo)
{
	return combo % 625 == 0 || combo == 3300 || combo == 4400 || combo == 33 || combo == 44;
}

static void worker(void)
{
	for (int next; (next = __atomic_fetch_add(&sh->next_combo, 1, __ATOMIC_RELAXED)) < COMBOS;) {
		int combo = sh->combo_order[next];
		int types[4], mode = cross_check(combo) ? FAST_DIGEST : FAST;
		struct page_memo memo = { .valid = 0 };
		combo_types(combo, types);
		for (unsigned i = 0; i < VARIANTS; i++) {
			struct result exact;
			run_case(types, &variants[i], &sh->results[combo][i], &memo, mode);
			if (mode == FAST)
				continue;
			memset(&exact, 0, sizeof(exact));	/* padding, compared below */
			run_case(types, &variants[i], &exact, &memo, EXACT_DIGEST);
			__atomic_fetch_add(&sh->cross_checked, 1, __ATOMIC_RELAXED);
			if (memcmp(&exact, &sh->results[combo][i], sizeof(exact)))
				__atomic_fetch_add(&sh->cross_mismatches, 1, __ATOMIC_RELAXED);
		}
	}
}

/* Oracle line: banks 0-3, L2 KB, L2 banks, memory KB, cache, 50h&0f 52h&7f 53h&3f 61h&30 ("-": not known) */
static int check_oracle(char *line, int lineno)
{
	char *tok[12];
	int n = 0, bad = 0;

	for (char *t = strtok(line, " \t\n"); t && n < 12; t = strtok(NULL, " \t\n"))
		tok[n++] = t;
	if (n == 0)
		return 0;
	if (n != 12) {
		printf("FAIL oracle line %d: %d fields, 12 expected\n", lineno, n);
		return 1;
	}
	int types[4];
	for (int bank = 0; bank < 4; bank++)
		types[bank] = atoi(tok[bank]);
	struct variant v = { REAL, { atoi(tok[4]), atoi(tok[5]), L2_DATA_REAL, 0, 0 }, 1, 0, 1 };
	struct page_memo memo = { .valid = 0 };
	struct result r;
	if (!v.hw.kb)
		v = variants[0];
	run_case(types, &v, &r, &memo, FAST);
	const struct { const char *name; unsigned mask, got; const char *want; } fields[] = {
		{ "memory KB", ~0u, r.top >> 10, tok[6] }, { "50h&0f", 0x0f, r.r50, tok[8] },
		{ "52h&7f", 0x7f, r.r52, tok[9] }, { "53h&3f", 0x3f, r.r53, tok[10] },
		{ "61h&30", 0x30, r.r61, tok[11] },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(fields); i++) {
		if (!strcmp(fields[i].want, "-"))
			continue;
		unsigned want = strtoul(fields[i].want, NULL, i ? 16 : 10);
		if ((fields[i].got & fields[i].mask) != want) {
			printf("FAIL oracle %s %s %s %s L2 %s/%s: %s %x, oracle %x\n", tok[0], tok[1], tok[2], tok[3],
			       tok[4], tok[5], fields[i].name, fields[i].got & fields[i].mask, want);
			bad = 1;
		}
	}
	return bad;
}

static void print_combo(const char *what, const int types[4])
{
	const struct result *res = sh->results[combo_of(types)];
	unsigned failing = 0;

	for (unsigned i = 0; i < VARIANTS; i++)
		failing += res[i].fails != 0;
	printf("%-6s %d %d %d %d: top %u MB (SIMMs give %u MB), 52h=%02x 53h=%02x 61h=%02x, L2 variants failing: %u of %d\n",
	       what, types[0], types[1], types[2], types[3], res[0].top >> 20, res[0].expected >> 20, res[0].r52,
	       res[0].r53, res[0].r61, failing, VARIANTS);
}

static void print_kinds(unsigned mask)
{
	for (int kind = 0; kind < FAIL_KINDS; kind++)
		if (mask & (1 << kind))
			printf(" [%s]", kind_names[kind]);
}

static void print_variant(FILE *f, const struct variant *v)
{
	fprintf(f, "L2 %s %uK/%d seed %u, setup L2 %d, CMOS %s, BOOTFAIL %u", class_names[v->class], v->hw.kb,
		v->hw.banks, v->hw.seed, v->l2_setting, v->defaulted ? "defaulted" : "valid", v->bootfail);
}

int main(int argc, char **argv)
{
	FILE *list = argc > 1 ? fopen(argv[1], "w") : NULL;
	char line[256];
	int oracle_lines = 0, oracle_fails = 0;

	if (argc > 1 && !list) {
		perror(argv[1]);
		return 2;
	}
	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	build_variants();
	m = model_new();
	compute_best_pairs();
	for (int lineno = 1; fgets(line, sizeof(line), stdin); lineno++) {
		oracle_lines += strspn(line, " \t\n") != strlen(line);
		oracle_fails += check_oracle(line, lineno);
	}

	int ordered = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int combo = 0; combo < COMBOS; combo++)
			if (cross_check(combo) == !pass)
				sh->combo_order[ordered++] = combo;

	long workers = sysconf(_SC_NPROCESSORS_ONLN);
	struct timespec t0, t1;
	if (workers < 1 || workers > 256)
		fatal("CPUs", workers);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long i = 0; i < workers; i++) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 2;
		}
		if (!pid) {
			worker();
			_exit(0);
		}
	}
	int worker_failures = 0;
	for (long i = 0; i < workers; i++) {
		int status;
		if (wait(&status) < 0) {
			perror("wait");
			return 2;
		}
		worker_failures += !WIFEXITED(status) || WEXITSTATUS(status);
	}
	if (worker_failures)
		fatal("worker processes failed:", worker_failures);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	unsigned cases = COMBOS * VARIANTS;

	printf("%u cases (%d SIMM combinations x %d L2 variants) in %.2f s: %.0f cases/s, %ld workers\n", cases,
	       COMBOS, VARIANTS, seconds, cases / seconds, workers);
	printf("oracle: %d of %d lines match\n", oracle_lines - oracle_fails, oracle_lines);
	printf("bulk reads: %u of %u cross-checked cases equal to one-by-one reads\n",
	       sh->cross_checked - sh->cross_mismatches, sh->cross_checked);
	print_combo("board", (const int[4]){ 0, 0, 3, 3 });
	print_combo("board", (const int[4]){ 0, 0, 4, 4 });
	print_combo("", (const int[4]){ 3, 3, 0, 0 });
	print_combo("", (const int[4]){ 4, 4, 0, 0 });

	unsigned class_cases[CLASSES] = { 0 }, class_fails[CLASSES] = { 0 }, class_kinds[CLASSES] = { 0 };
	int class_example[CLASSES][2];
	unsigned failing = 0;
	for (int combo = 0; combo < COMBOS; combo++)
		for (unsigned i = 0; i < VARIANTS; i++) {
			const struct result *r = &sh->results[combo][i];
			const struct variant *v = &variants[i];
			class_cases[v->class]++;
			if (!r->fails)
				continue;
			if (!class_fails[v->class]++) {
				class_example[v->class][0] = combo;
				class_example[v->class][1] = i;
			}
			class_kinds[v->class] |= r->fails;
			failing++;
			if (list) {
				int types[4];
				combo_types(combo, types);
				fprintf(list, "%d %d %d %d ", types[0], types[1], types[2], types[3]);
				print_variant(list, v);
				fprintf(list, ": fails %03x top %u MB SIMMs %u MB 50h=%02x (coreboot %02x) 52h=%02x 53h=%02x "
					"61h=%02x first bad address %08x\n", r->fails, r->top >> 20, r->expected >> 20, r->r50,
					r->r50_coreboot, r->r52, r->r53, r->r61, r->bad_addr);
			}
		}
	if (list && fclose(list)) {
		perror(argv[1]);
		return 2;
	}
	printf("%u of %u cases fail\n", failing, cases);
	for (int c = 0; c < CLASSES; c++) {
		printf("  L2 %-29s %7u of %7u cases fail", class_names[c], class_fails[c], class_cases[c]);
		if (class_fails[c]) {
			int types[4];
			combo_types(class_example[c][0], types);
			print_kinds(class_kinds[c]);
			printf(", e.g. \"%d %d %d %d\" ", types[0], types[1], types[2], types[3]);
			print_variant(stdout, &variants[class_example[c][1]]);
		}
		printf("\n");
	}
	return (failing || oracle_fails || sh->cross_mismatches || !sh->cross_checked) ? 1 : 0;
}
