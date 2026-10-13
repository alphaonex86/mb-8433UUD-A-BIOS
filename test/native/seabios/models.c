/* Board models shared by the areas: CMOS/RTC (70h/71h) and PCI configuration mechanism #1 (CF8h/CFCh)
   with BAR sizing, expansion ROM decoding and the UMC host bridge shadow register 54h, whose bits 2-7
   route the 16 KiB blocks of C8000-DFFFF to the ISA bus (cleared) or to shadow RAM (set). */
#define _GNU_SOURCE
#include <string.h>
#include <sys/mman.h>

#include "devfuzz.h"

#define HOST_BRIDGE 0x80                        /* 0:10.0 */

unsigned char fuzz_cmos[128];
static unsigned char CmosIndex;

static unsigned cmos_read(unsigned short port, int size)
{
	return port == 0x71 ? fuzz_cmos[CmosIndex] : 0xff;
}

static void cmos_write(unsigned short port, unsigned value, int size)
{
	if (port == 0x70)
		CmosIndex = value & 0x7f;
	else if (CmosIndex == 0x0a)
		fuzz_cmos[0x0a] = (fuzz_cmos[0x0a] & 0x80) | (value & 0x7f);   /* UIP is read-only */
	else if (CmosIndex != 0x0c && CmosIndex != 0x0d)
		fuzz_cmos[CmosIndex] = value;
}

void fuzz_cmos_model(void)
{
	static const struct fuzz_ports ports = { 0x70, 0x71, cmos_read, cmos_write };
	fuzz_cmos[0x0a] = 0x26;                     /* UIP clear */
	fuzz_cmos[0x0b] = 0x02;                     /* BCD, 24 h */
	fuzz_cmos[0x0d] = 0x80;                     /* battery good */
	fuzz_cmos[0x07] = 0x12, fuzz_cmos[0x08] = 0x10, fuzz_cmos[0x09] = 0x26, fuzz_cmos[0x32] = 0x20;
	fuzz_claim(&ports);
}

struct fuzz_pcidev fuzz_pci[FUZZ_PCI_MAX];
int fuzz_npci;
static unsigned PciAddress;
unsigned char fuzz_isa_image[0x18000];
static unsigned char RamBehindIsa[0x18000];

struct fuzz_pcidev *fuzz_pci_add(unsigned bdf, unsigned vendev, unsigned classrev, unsigned char header)
{
	if (fuzz_npci == FUZZ_PCI_MAX)
		fuzz_fail("harness: too many PCI devices");
	struct fuzz_pcidev *d = &fuzz_pci[fuzz_npci++];
	memset(d, 0, sizeof(*d));
	d->bdf = bdf;
	memcpy(&d->cfg[0x00], &vendev, 4);
	memcpy(&d->cfg[0x08], &classrev, 4);
	d->cfg[0x0e] = header;
	return d;
}

void fuzz_pci_board(void)
{
	struct fuzz_pcidev *hb = fuzz_pci_add(HOST_BRIDGE, 0x88811060, 0x06000000, 0);
	hb->cfg[0x50] = 0x82;                       /* L2 256K on */
	hb->cfg[0x54] = 0xfe;                       /* C0000-DFFFF: shadow RAM */
	fuzz_pci_add(0x90, 0x886a1060, 0x06010000, 0x80);
	struct fuzz_pcidev *ide = fuzz_pci_add(0x91, 0x673a1060, 0x01018000, 0);
	ide->cfg[0x41] = 0xc0;                      /* both channels on */
}

static struct fuzz_pcidev *pci_find(unsigned bdf)
{
	for (int i = 0; i < fuzz_npci; i++) {
		struct fuzz_pcidev *d = &fuzz_pci[i];
		if (d->bdf == bdf || (d->all_functions && (d->bdf & ~7) == (bdf & ~7)))
			return d;
	}
	return NULL;
}

unsigned fuzz_pci_read(unsigned bdf, unsigned reg)
{
	struct fuzz_pcidev *d = pci_find(bdf);
	unsigned v = 0xffffffff;
	if (d)
		memcpy(&v, &d->cfg[reg & 0xfc], 4);
	return v;
}

/* Blocks whose 54h bit changed now read from the other side; each side keeps its contents. */
static void route_isa(unsigned char old, unsigned char now)
{
	for (int block = 0; block < 6; block++) {
		int isa = !(now & 4 << block);
		unsigned char *window = fuzz_phys(0xc8000 + block * 0x4000);
		if (isa == !(old & 4 << block))
			continue;
		unsigned char *save = isa ? &RamBehindIsa[block * 0x4000] : &fuzz_isa_image[block * 0x4000];
		unsigned char *load = isa ? &fuzz_isa_image[block * 0x4000] : &RamBehindIsa[block * 0x4000];
		memcpy(save, window, 0x4000);
		memcpy(window, load, 0x4000);
	}
}

static void rom_decode(struct fuzz_pcidev *d)
{
	unsigned bar;
	memcpy(&bar, &d->cfg[0x30], 4);
	unsigned addr = bar & 0xfffff800, len = (d->rom_len + 0xfff) & ~0xfff;
	if (d->rom_mapped && (!(bar & 1) || addr != d->rom_mapped)) {
		munmap((void *)d->rom_mapped, len);
		d->rom_mapped = 0;
	}
	if (!(bar & 1) || !addr || !d->rom || d->rom_mapped || addr & 0xfff)
		return;
	void *at = (void *)addr;
	if (addr < FUZZ_PHYS_END || (unsigned long long)addr + len > 0xff000000ULL
	    || mmap(at, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != at) {
		fuzz_note("ROM BAR %08x: cannot decode there (host memory), reads float", addr);
		return;
	}
	memset(at, 0xff, len);
	memcpy(at, d->rom, d->rom_len);
	d->rom_mapped = addr;
}

void (*fuzz_pci_command_hook)(unsigned bdf, unsigned command);

static void config_write(struct fuzz_pcidev *d, unsigned reg, unsigned value, int size)
{
	unsigned char old54 = d->cfg[0x54];
	for (int i = 0; i < size; i++) {
		unsigned r = reg + i, byte = value >> (8 * i) & 0xff;
		if (r < 0x04 || (r >= 0x08 && r < 0x0c) || r == 0x0e)
			continue;                           /* read-only identification */
		if ((r >= 0x10 && r < 0x28) || (r >= 0x30 && r < 0x34)) {
			int bar = r >= 0x30 ? 6 : (r - 0x10) / 4;
			unsigned char mask = d->bar_mask[bar] >> (8 * (r & 3));
			d->cfg[r] = (d->cfg[r] & ~mask) | (byte & mask);
		} else {
			d->cfg[r] = byte;
		}
	}
	if (reg <= 0x33 && reg + size > 0x30)
		rom_decode(d);
	if (fuzz_pci_command_hook && reg <= 0x05 && reg + size > 0x04)
		fuzz_pci_command_hook(d->bdf, d->cfg[0x04] | d->cfg[0x05] << 8);
	if (d->bdf == HOST_BRIDGE && reg <= 0x54 && reg + size > 0x54) {
		route_isa(old54, d->cfg[0x54]);
	}
}

static unsigned pci_port_read(unsigned short port, int size)
{
	if (port >= 0xcf8 && port < 0xcfc)
		return size == 4 ? PciAddress : 0xffffffff;
	if (!(PciAddress & 0x80000000))
		return 0xffffffff;
	unsigned shift = 8 * (port & 3);
	return fuzz_pci_read(PciAddress >> 8 & 0xffff, PciAddress & 0xfc) >> shift;
}

static void pci_port_write(unsigned short port, unsigned value, int size)
{
	if (port == 0xcf8 && size == 4) {
		PciAddress = value;
		return;
	}
	struct fuzz_pcidev *d = pci_find(PciAddress >> 8 & 0xffff);
	if (port >= 0xcfc && d && (PciAddress & 0x80000000))
		config_write(d, (PciAddress & 0xfc) + (port & 3), value, size);
}

void fuzz_pci_model(void)
{
	static const struct fuzz_ports ports = { 0xcf8, 0xcff, pci_port_read, pci_port_write };
	fuzz_claim(&ports);
}
