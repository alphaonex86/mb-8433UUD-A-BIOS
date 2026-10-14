/* Device fault-injection harness for SeaBIOS (newBIOS) compiled on the host: shared by the runner (libc
   side) and the device models (SeaBIOS side), so only plain C types here. */
#ifndef DEVFUZZ_H
#define DEVFUZZ_H

#include <stdarg.h>

#define FUZZ_PHYS_LOW 0x10000                   /* physical 0x10000-0x10ffff is mapped at the same host address */
#define FUZZ_PHYS_END 0x110000

/* Virtual time: every port access, delay and timer poll moves it; past the budget the case hangs. */
extern unsigned long long fuzz_now_us, fuzz_budget_us;
extern unsigned long long fuzz_post_us;          /* POST time the case reports (default: all of it) */
void fuzz_tick(unsigned long long us);

/* A case ends with fuzz_fail (wrong behaviour) or fuzz_hang; returning from run() is a pass. */
enum { FUZZ_PASS, FUZZ_FAIL, FUZZ_HANG };
void fuzz_fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void fuzz_hang(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void fuzz_exit(int status, const char *fmt, va_list args) __attribute__((noreturn));
extern int fuzz_verbose;
void fuzz_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

unsigned fuzz_rand(void);
void fuzz_seed(unsigned seed);

/* I/O ports: a model claims a range; reads of unclaimed ports float (all ones). */
struct fuzz_ports {
	unsigned short lo, hi;
	unsigned (*read)(unsigned short port, int size);
	void (*write)(unsigned short port, unsigned value, int size);
};
void fuzz_claim(const struct fuzz_ports *ports);
void fuzz_irq_hook(void (*poll)(void));        /* called while the BIOS waits (yield): interrupt delivery */
struct fuzz_mmio {                              /* memory-mapped registers at base..base+size */
	unsigned base, size;
	unsigned (*read)(unsigned offset, int size);
	void (*write)(unsigned offset, unsigned value, int size);
};
void fuzz_claim_mmio(const struct fuzz_mmio *mmio);
void fuzz_map_phys(unsigned base, unsigned size);   /* host memory at a physical address */

/* Physical memory 0-0x10ffff (below 64 KiB: host buffer), ISA bus view of C8000-DFFFF. */
void *fuzz_phys(unsigned addr);
extern unsigned char fuzz_isa_image[0x18000];

int fuzz_screen_has(const char *text);          /* printed by the BIOS during the case */
extern const unsigned short *fuzz_keys;         /* keyboard script, then longjmp to fuzz_keys_done */
extern int fuzz_nkeys;
extern void **fuzz_keys_done;

/* Boot devices registered by the drivers. */
struct fuzz_boot {
	char kind;                                  /* 'H' disk, 'C' CD, 'F' floppy, 'V' BCV, 'E' BEV */
	void *drive;
	unsigned seg, ip;
};
extern struct fuzz_boot fuzz_boots[512];
extern int fuzz_nboots;
extern unsigned fuzz_romcalls[32];              /* seg:off of the option ROMs run */
extern int fuzz_nromcalls;

/* CMOS (ports 70h/71h). */
extern unsigned char fuzz_cmos[128];
void fuzz_cmos_model(void);

/* PCI configuration space (ports CF8h/CFCh). */
#define FUZZ_PCI_MAX 32
struct fuzz_pcidev {
	unsigned bdf;
	unsigned char cfg[256];
	unsigned bar_mask[7];                       /* size mask of BARs 0-5 and the ROM BAR (30h) */
	unsigned all_functions;                     /* answers on every function of its slot */
	unsigned char *rom;                         /* image at the ROM BAR address while enabled */
	unsigned rom_len;
	unsigned rom_mapped;
};
extern struct fuzz_pcidev fuzz_pci[FUZZ_PCI_MAX];
extern int fuzz_npci;
struct fuzz_pcidev *fuzz_pci_add(unsigned bdf, unsigned vendev, unsigned classrev, unsigned char header);
void fuzz_pci_board(void);                      /* UMC host bridge, IBC and onboard IDE */
void fuzz_pci_model(void);
unsigned fuzz_pci_read(unsigned bdf, unsigned reg);
extern void (*fuzz_pci_command_hook)(unsigned bdf, unsigned command);   /* on each write of the command register */

/* Option ROM image description: every field may be wrong. */
struct fuzz_rom {
	unsigned char sig_bad, size, pcir_bad, type, indicator, checksum_bad, images;
	unsigned char pnp;                          /* 0 none, 1 $PnP header, 2 header with a bad signature */
	unsigned short pcioffset, pnpoffset, vendor, device, ilen, nextoffset, bev, bcv;
};
void fuzz_rom_default(struct fuzz_rom *r, unsigned short vendor, unsigned short device, unsigned char blocks);
void fuzz_rom_build(unsigned char *img, int room, const struct fuzz_rom *r);

/* Entry points of the code under test (SeaBIOS side). */
void fuzz_bios_init(void);                      /* BDA, IVT, timer: what POST set up before */
void fuzz_setup_init(void);                     /* setup.c setup_init(): CMOS settings */
void fuzz_setup_usb_text(char *out, int size);    /* the setup's USB page */
void fuzz_setup_screens(void);                  /* POST and setup screens that show the devices */
int fuzz_setup_disk_bench(void);                /* the setup's disk benchmark on each hard disk: how many passed */
void fuzz_cpu_allow(unsigned us);               /* more CPU time for the rest of the case */
void fuzz_setup_isapnp(void);
void fuzz_setup_pnp(void);
void fuzz_setup_post_warnings(void);
int fuzz_setting(int index);
void fuzz_set_setting(int index, int value);
void fuzz_cmos_store(void);                     /* setup.c cmos_write() of the settings */
void fuzz_set_hdd(int idx, int type, unsigned cyl, unsigned heads, unsigned spt);
struct fuzz_ide {                               /* setup.c IdeInfo[]: what the POST and setup show */
	unsigned present, atapi, sectors, cyl, heads, spt, pio, pio_max, multiple;
	const char *model;
	void *drive;
};
void fuzz_ideinfo(int idx, struct fuzz_ide *out);
int fuzz_setting_max(int var);
const char *fuzz_check_settings(char *why, int size);
const char *fuzz_check_defaults(char *why, int size);
void fuzz_cmos_fix_sums(void);
int fuzz_cmos_valid(void);
unsigned short fuzz_password_hash(const char *s);
unsigned short fuzz_password(void);
void fuzz_setup_menu(void);
void fuzz_setup_boot_password(void);
void fuzz_setup_post(void);
unsigned fuzz_setup_memtest(int quick);          /* setup.c memtest() on fuzz_free: its first bad address */
extern void (*fuzz_wbinvd_hook)(void);          /* called by wbinvd() */
extern unsigned fuzz_free[4][2];                /* malloc_tmphigh_free(): the free RAM ranges */
extern int fuzz_nfree;

struct fuzz_area {
	const char *name;
	int nclasses;
	const char *const *class_names;
	unsigned (*count)(void);
	int (*class_of)(unsigned index);
	void (*describe)(unsigned index, char *buf, int size);
	void (*run)(unsigned index);
};
extern const struct fuzz_area fuzz_ata, fuzz_fdc, fuzz_pcirom, fuzz_isa, fuzz_usb, fuzz_usbhc, fuzz_cmos_area, fuzz_pnp, fuzz_memtest;

#endif
