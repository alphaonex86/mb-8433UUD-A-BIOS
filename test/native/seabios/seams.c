/* Link-time seams: everything the SeaBIOS files under test call outside of themselves. Port I/O goes to
   the device models, time is virtual, option ROMs and real-mode calls are recorded instead of run. */
#include "biosvar.h"
#include "block.h"
#include "bregs.h"
#include "e820map.h"
#include "hw/pci.h"
#include "hw/pcidevice.h"
#include "malloc.h"
#include "output.h"
#include "romfile.h"
#include "stacks.h"
#include "std/disk.h"
#include "std/optionrom.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

int sb_vsnprintf(char *str, size_t size, const char *fmt, va_list args);   /* output_seam.c */
char *strstr(const char *haystack, const char *needle);
int posix_memalign(void **ptr, size_t align, size_t size);

/****************************************************************
 * Port I/O
 ****************************************************************/

static const struct fuzz_ports *Ports[16];
static int NPorts;
static void (*IrqPoll)(void);

void fuzz_claim(const struct fuzz_ports *ports)
{
	if (NPorts == ARRAY_SIZE(Ports))
		fuzz_fail("harness: too many port ranges");
	Ports[NPorts++] = ports;
}

void fuzz_irq_hook(void (*poll)(void))
{
	IrqPoll = poll;
}

static const struct fuzz_ports *port_owner(u16 port)
{
	int i;
	for (i = 0; i < NPorts; i++)
		if (port >= Ports[i]->lo && port <= Ports[i]->hi)
			return Ports[i];
	return NULL;
}

static u32 port_in(u16 port, int size)
{
	fuzz_tick(1);
	const struct fuzz_ports *p = port_owner(port);
	u32 mask = size == 4 ? 0xffffffff : (1u << (8 * size)) - 1;
	return (p ? p->read(port, size) : 0xffffffff) & mask;
}

static void port_out(u16 port, u32 value, int size)
{
	fuzz_tick(1);
	const struct fuzz_ports *p = port_owner(port);
	if (p)
		p->write(port, value, size);
}

u8 inb(u16 port) { return port_in(port, 1); }
u16 inw(u16 port) { return port_in(port, 2); }
u32 inl(u16 port) { return port_in(port, 4); }
void outb(u8 value, u16 port) { port_out(port, value, 1); }
void outw(u16 value, u16 port) { port_out(port, value, 2); }
void outl(u32 value, u16 port) { port_out(port, value, 4); }

void insb(u16 port, u8 *data, u32 count) { while (count--) *data++ = inb(port); }
void insw(u16 port, u16 *data, u32 count) { while (count--) *data++ = inw(port); }
void insl(u16 port, u32 *data, u32 count) { while (count--) *data++ = inl(port); }
void outsb(u16 port, u8 *data, u32 count) { while (count--) outb(*data++, port); }
void outsw(u16 port, u16 *data, u32 count) { while (count--) outw(*data++, port); }
void outsl(u16 port, u32 *data, u32 count) { while (count--) outl(*data++, port); }

/* MMIO: registers of a model, else plain memory. The code also reads some registers without readl():
   those get the last value written. */
static const struct fuzz_mmio *Mmio[8];
static int NMmio;

void fuzz_claim_mmio(const struct fuzz_mmio *mmio)
{
	if (NMmio == ARRAY_SIZE(Mmio))
		fuzz_fail("harness: too many MMIO ranges");
	Mmio[NMmio++] = mmio;
	fuzz_map_phys(mmio->base, mmio->size);      /* direct register reads see the last value written */
}

static const struct fuzz_mmio *mmio_owner(const void *addr)
{
	for (int i = 0; i < NMmio; i++)
		if ((u32)addr - Mmio[i]->base < Mmio[i]->size)
			return Mmio[i];
	return NULL;
}

static u32 mmio_read(const void *addr, int size)
{
	const struct fuzz_mmio *m = mmio_owner(addr);
	if (!m) {
		u32 v = 0;
		memcpy(&v, addr, size);
		return v;
	}
	fuzz_tick(1);
	return m->read((u32)addr - m->base, size);
}

static void mmio_write(void *addr, u32 value, int size)
{
	const struct fuzz_mmio *m = mmio_owner(addr);
	if (!m) {
		memcpy(addr, &value, size);
		return;
	}
	fuzz_tick(1);
	memcpy(addr, &value, size);
	m->write((u32)addr - m->base, value, size);
}

void writel(void *addr, u32 val) { mmio_write(addr, val, 4); }
void writew(void *addr, u16 val) { mmio_write(addr, val, 2); }
void writeb(void *addr, u8 val) { mmio_write(addr, val, 1); }
u32 readl(const void *addr) { return mmio_read(addr, 4); }
u16 readw(const void *addr) { return mmio_read(addr, 2); }
u8 readb(const void *addr) { return mmio_read(addr, 1); }
u64 readq(const void *addr) { return readl(addr) | (u64)readl(addr + 4) << 32; }

/****************************************************************
 * CPU
 ****************************************************************/

static u32 Cr0 = 0x10;

void irq_disable(void) { }
void irq_enable(void) { }
u32 save_flags(void) { return F_IF; }
void restore_flags(u32 flags) { }
void cpu_relax(void) { fuzz_tick(1); }
void hlt(void) { yield(); }
void wbinvd(void) { }
u32 cr0_read(void) { return Cr0; }
void cr0_write(u32 cr0) { Cr0 = cr0; }
void cr0_mask(u32 off, u32 on) { Cr0 = (Cr0 & ~off) | on; }
u16 cr0_vm86_read(void) { return Cr0; }
u64 rdmsr(u32 index) { fuzz_fail("rdmsr %x on a 486", index); }
void wrmsr(u32 index, u64 val) { fuzz_fail("wrmsr %x on a 486", index); }
u64 rdtscll(void) { fuzz_fail("rdtsc on a 486"); }
u32 getesp(void) { return 0x7000; }
void sgdt(struct descloc_s *desc) { }
void lgdt(struct descloc_s *desc) { }
u8 get_a20(void) { return 1; }
u8 set_a20(u8 cond) { return 1; }

void __cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	*eax = index ? 0x484 : 1;                   /* Am486DX4: family 4 */
	*ebx = index ? 0 : 0x68747541;
	*ecx = index ? 0 : 0x444d4163;
	*edx = index ? 1 : 0x69746e65;
}

void cpuid(u32 index, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	__cpuid(index, eax, ebx, ecx, edx);
}

/****************************************************************
 * Time and threads
 ****************************************************************/

u32 TimerKHz = 1000;

u32 timer_read(void) { fuzz_tick(1); return fuzz_now_us; }
u32 timer_calc_usec(u32 usecs) { return timer_read() + usecs; }
u32 timer_calc(u32 msecs) { return timer_calc_usec(msecs * 1000); }
int timer_check(u32 end) { return (s32)(timer_read() - end) > 0; }
void ndelay(u32 count) { fuzz_tick((count + 999) / 1000); }
void udelay(u32 count) { fuzz_tick(count); }
void mdelay(u32 count) { fuzz_tick(count * 1000ULL); }
void nsleep(u32 count) { ndelay(count); }
void usleep(u32 count) { udelay(count); }
void msleep(u32 count) { mdelay(count); }

/* One yield lets the other threads and the interrupts run: about a millisecond. */
void yield(void)
{
	fuzz_tick(1000);
	if (IrqPoll)
		IrqPoll();
}

void yield_toirq(void) { yield(); }

/* Threads run one after the other, each on its own clock from its start; wait_threads joins them. */
static unsigned long long ThreadsEnd;

void run_thread(void (*func)(void *), void *data)
{
	unsigned long long start = fuzz_now_us;
	func(data);
	if (fuzz_now_us > ThreadsEnd)
		ThreadsEnd = fuzz_now_us;
	fuzz_now_us = start;
}

void wait_threads(void)
{
	if (ThreadsEnd > fuzz_now_us)
		fuzz_now_us = ThreadsEnd;
}
u32 ticks_to_ms(u32 ticks) { return ticks / 1000; }
u32 ticks_from_ms(u32 ms) { return ms * 1000ULL > 0xffffffff ? 0xffffffff : ms * 1000; }
void pit_setup(void) { }

u32 __call32_params(void *func, u32 eax, u32 edx, u32 ecx, u32 errret)
{
	return ((u32 (*)(u32, u32, u32))func)(eax, edx, ecx);
}

void mutex_lock(struct mutex_s *mutex)
{
	while (mutex->isLocked)
		yield();
	mutex->isLocked = 1;
}

void mutex_unlock(struct mutex_s *mutex) { mutex->isLocked = 0; }
void start_preempt(void) { }
void finish_preempt(void) { }
int wait_preempt(void) { return 0; }
void check_preempt(void) { }

/****************************************************************
 * Memory
 ****************************************************************/

static u8 LowMem[FUZZ_PHYS_LOW];
u16 __segment_ES, __segment_CS, __segment_DS, __segment_SS;
u8 _zonelow_seg[1];

void *fuzz_phys(unsigned addr)
{
	return addr < FUZZ_PHYS_LOW ? (void *)&LowMem[addr] : (void *)addr;
}

void *seam_flatptr(u32 seg, u32 off)
{
	return fuzz_phys(seg * 16 + off);
}

struct zone_s { int unused; } ZoneLow, ZoneHigh, ZoneFSeg, ZoneTmpLow, ZoneTmpHigh;

void *_malloc(struct zone_s *zone, u32 size, u32 align)
{
	void *ptr;
	if (size > 4 << 20)
		return NULL;
	if (posix_memalign(&ptr, align < sizeof(void *) ? sizeof(void *) : align, size ?: 1))
		fuzz_fail("harness: out of memory");
	memset(ptr, 0xa5, size);                    /* SeaBIOS memory is not cleared either */
	return ptr;
}

int malloc_tmphigh_free(int n, u32 *start, u32 *end) { return 0; }

static u32 RomEnd = BUILD_ROM_START;

u32 rom_get_max(void) { return FUZZ_ROM_MAX; }
u32 rom_get_last(void) { return RomEnd; }
void rom_set_last(u32 newend) { RomEnd = newend; }

struct rom_header *rom_reserve(u32 size)
{
	u32 newend = ALIGN(RomEnd + size, OPTION_ROM_ALIGN);
	if (newend > rom_get_max())
		return NULL;
	return (void *)RomEnd;
}

int rom_confirm(u32 size)
{
	if (!rom_reserve(size)) {
		warn_noalloc();
		return -1;
	}
	RomEnd = ALIGN(RomEnd + size, OPTION_ROM_ALIGN);
	return 0;
}

int dma_floppy(u32 addr, int count, int isWrite)
{
	if ((addr & 0xffff) + count > 0x10000 || addr + count > 0x1000000)
		return -1;                              /* crosses a 64 KiB DMA page */
	fuzz_note("dma %x+%x %s", addr, count, isWrite ? "write" : "read");
	return 0;
}

/****************************************************************
 * Output
 ****************************************************************/

static char Screen[1 << 16];                   /* teletype output of the case */
static int ScreenLen;

static void screen_put(const char *fmt, va_list args)
{
	int len = sb_vsnprintf(Screen + ScreenLen, sizeof(Screen) - ScreenLen, fmt, args);
	fuzz_tick(len > 0 ? len : 0);
	if (fuzz_verbose && len > 0)
		fuzz_note("screen: %.*s", len, Screen + ScreenLen);
	if (len > 0)
		ScreenLen = ScreenLen + len < (int)sizeof(Screen) / 2 ? ScreenLen + len : 0;
}

int fuzz_screen_has(const char *text)
{
	return strstr(Screen, text) != NULL;
}

void printf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	screen_put(fmt, args);
	va_end(args);
}

void __dprintf(const char *fmt, ...) { }

void panic(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fuzz_exit(FUZZ_FAIL, fmt, args);
}

/* setup.c's L2 asm: 50h written as on the board; timings 2-3 "do not cache", to reach both results */
void l2_fill(u32 base, u32 dwords, u8 hb50) { pci_config_writeb(pci_to_bdf(0, 0x10, 0), 0x50, hb50); }
void l2_fill_off(u32 base, u32 dwords, u8 hb50) { pci_config_writeb(pci_to_bdf(0, 0x10, 0), 0x50, hb50 & 0x30); }
u32 l2_line_works(u32 addr, u32 size, u8 hb50) { return 1; }
u32 l2_caches(u32 addr, u32 size, u8 hb50) { return !(hb50 & 0x20); }

void __warn_timeout(int lineno, const char *fname) { fuzz_note("timeout %s:%d", fname, lineno); }
void __warn_noalloc(int lineno, const char *fname) { fuzz_note("out of memory %s:%d", fname, lineno); }
void __warn_internalerror(int lineno, const char *fname) { fuzz_note("internal error %s:%d", fname, lineno); }
void reset(void)
{
	if (fuzz_keys_done)
		__builtin_longjmp(fuzz_keys_done, 1);
	fuzz_fail("reset() called");
}

/****************************************************************
 * Real mode, interrupts, option ROMs
 ****************************************************************/

void irq_trampoline_0x10(void) { }
void irq_trampoline_0x1c(void) { }
void irq_trampoline_0x4a(void) { }
void entry_08(void) { }
void entry_70(void) { }
void ps2_check_event(void) { }
void sercon_check_event(void) { }
void __set_invalid(struct bregs *regs, int lineno, const char *fname) { }
void __set_unimplemented(struct bregs *regs, int lineno, const char *fname) { }
void __set_code_invalid(struct bregs *regs, u32 linecode, const char *fname) { }
void __set_code_unimplemented(struct bregs *regs, u32 linecode, const char *fname) { }
void entry_0e(void) { }
void entry_76(void) { }
void entry_19_official(void) { }
void enable_hwirq(int hwirq, struct segoff_s func) { }
void __call16_int(struct bregs *callregs, u16 offset) { fuzz_tick(100); }
void entry_pnp_real(void) { }
void entry_pnp_prot(void) { }
void tpm_option_rom(const void *addr, u32 len) { }
void vgahook_setup(struct pci_device *pci) { }

unsigned fuzz_romcalls[32];
int fuzz_nromcalls;

void farcall16big(struct bregs *callregs)
{
	if (fuzz_nromcalls == ARRAY_SIZE(fuzz_romcalls))
		fuzz_hang("more than %d option ROM calls", fuzz_nromcalls);
	fuzz_romcalls[fuzz_nromcalls++] = callregs->code.segoff;
	fuzz_tick(1000);
}


/****************************************************************
 * Boot list, CBFS, keyboard, other drivers
 ****************************************************************/

struct fuzz_boot fuzz_boots[512];
int fuzz_nboots;

static void boot_add(char kind, void *drive, unsigned seg, unsigned ip)
{
	if (fuzz_nboots == ARRAY_SIZE(fuzz_boots))
		fuzz_hang("more than %d boot entries", fuzz_nboots);
	fuzz_boots[fuzz_nboots++] = (struct fuzz_boot){ kind, drive, seg, ip };
}

void boot_add_hd(struct drive_s *drive, const char *desc, int prio) { boot_add('H', drive, 0, 0); }
void boot_add_cd(struct drive_s *drive, const char *desc, int prio) { boot_add('C', drive, 0, 0); }
void boot_add_floppy(struct drive_s *drive, const char *desc, int prio) { boot_add('F', drive, 0, 0); }
void boot_add_bcv(u16 seg, u16 ip, u16 desc, int prio) { boot_add('V', NULL, seg, ip); }
void boot_add_bev(u16 seg, u16 bev, u16 desc, int prio) { boot_add('E', NULL, seg, bev); }
int boot_hd_list(struct drive_s **drive, const char **desc, int max)
{
	int n = 0;
	for (int b = 0; b < fuzz_nboots && n < max; b++)
		if (fuzz_boots[b].kind == 'H') {
			drive[n] = fuzz_boots[b].drive;
			desc[n++] = NULL;
		}
	return n;
}
int bootprio_find_ata_device(struct pci_device *pci, int chanid, int slave) { return -1; }
int bootprio_find_fdc_device(struct pci_device *pci, int port, int fdid) { return -1; }
int bootprio_find_named_rom(const char *name, int instance) { return -1; }
int bootprio_find_pci_rom(struct pci_device *pci, int instance) { return -1; }
int boot_lchs_find_ata_device(struct pci_device *pci, int chanid, int slave, struct chs_s *chs) { return -1; }

static struct romfile_s *RomFiles;            /* romfile_add()ed (the flash boot ROM, optionroms.c) */
void romfile_add(struct romfile_s *file) { file->next = RomFiles; RomFiles = file; }
struct romfile_s *romfile_find(const char *name)
{
	for (struct romfile_s *f = RomFiles; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	return NULL;
}
struct romfile_s *romfile_findprefix(const char *prefix, struct romfile_s *prev)
{
	for (struct romfile_s *f = prev ? prev->next : RomFiles; f; f = f->next)
		if (!memcmp(f->name, prefix, strlen(prefix)))
			return f;
	return NULL;
}
void *romfile_loadfile(const char *name, int *psize) { return NULL; }
u64 romfile_loadint(const char *name, u64 defval) { return defval; }

int PS2KeyboardOK = 1;
/* ps2port.c's reason for a keyboard error: its longest text, to check the POST line */
void ps2_kbd_failure(char *buf, int size) { snprintf(buf, size, " (%s, got %02x)", "8042 keyboard line test", 0xff); }
u8 ApmEnabled;
int ps2_kbd_command(int command, u8 *param) { return -1; }
/* Keys: the case's script (scan << 8 | ascii), then the user walks away (fuzz_keys_done); without a
   script, F1 at once (the POST error prompt). */
const u16 *fuzz_keys;
int fuzz_nkeys;
void **fuzz_keys_done;

int get_keystroke_full(int msec)
{
	if (!fuzz_keys) {
		fuzz_tick(msec * 1000ULL);
		return 0x3b00;
	}
	fuzz_tick(20000);
	if (fuzz_nkeys) {
		fuzz_nkeys--;
		return *fuzz_keys++;
	}
	if (fuzz_keys_done)
		__builtin_longjmp(fuzz_keys_done, 1);
	fuzz_fail("waiting for a key after the script");
}

int get_keystroke(int msec)
{
	int key = get_keystroke_full(msec);
	return key < 0 ? key : key >> 8;
}
const char VERSION[] = "devfuzz", BUILDINFO[] = "devfuzz", RELEASE_DATE[] = "01/01/2026";
struct e820entry e820_list[1];
int e820_count;
struct floppy_dbt_s diskette_param_table;

#define NO_DRIVER(name) int name(struct disk_op_s *op) { fuzz_fail(#name " called"); }
NO_DRIVER(ahci_process_op) NO_DRIVER(ahci_atapi_process_op) NO_DRIVER(esp_scsi_process_op)
NO_DRIVER(lsi_scsi_process_op) NO_DRIVER(megasas_process_op) NO_DRIVER(mpt_scsi_process_op)
NO_DRIVER(nvme_process_op) NO_DRIVER(pvscsi_process_op) NO_DRIVER(sdcard_process_op)
NO_DRIVER(uas_process_op) NO_DRIVER(virtio_blk_process_op)
NO_DRIVER(virtio_scsi_process_op)
void ahci_setup(void) { }
void esp_scsi_setup(void) { }
void lsi_scsi_setup(void) { }
void megasas_setup(void) { }
void mpt_scsi_setup(void) { }
void nvme_setup(void) { }
void pvscsi_setup(void) { }
void ramdisk_setup(void) { }
void sdcard_setup(void) { }
void virtio_blk_setup(void) { }
void virtio_scsi_setup(void) { }
int usb_uas_setup(struct usbdevice_s *usbdev) { return -1; }

/****************************************************************
 * POST state the code under test expects
 ****************************************************************/

void fuzz_bios_init(void)
{
	SET_BDA(ebda_seg, 0x9fc0);
	SET_BDA(mem_size_kb, 636);
	SET_BDA(port_com[0], 0x3f8);
	SET_BDA(port_com[1], 0x2f8);
	SET_BDA(port_lpt[0], 0x378);
	SET_BDA(video_mode, 3);
	SET_IVT(0x19, FUNC16(entry_19_official));
}
