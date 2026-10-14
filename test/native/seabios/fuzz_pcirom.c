/* PCI: devices with holes, aliases, bridges, odd header types, classes and BARs, and expansion ROMs with
   every header field wrong, run through pci_probe_devices(), vgarom_setup(), optionrom_setup() (ISA ROM
   scan included) and the setup's PCI page; a ROM in a 2 Mbit flash's lower half (unreachable on the board: A17 of
   its 1 Mbit socket is high) is never run, valid or not, whatever the NIC, and IBC 57h is left alone. */
#include "biosvar.h"
#include "hw/pci.h"
#include "hw/pcidevice.h"
#include "hw/pci_regs.h"
#include "malloc.h"
#include "output.h"
#include "std/optionrom.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

#define NDEV 6
#define ROM_WINDOW 0x18000000                   /* ROM BARs: free host addresses, 1 MiB per device */

struct devspec {
	u8 present, slot, fn, bus, header, all_functions, secondary;
	u16 vendor, device, class;
	u32 bar_mask[6], bar_val[6], rom_mask, rom_base;
	int has_rom;
	struct fuzz_rom rom;
};

enum { FL_NONE, FL_VALID, FL_BAD_SUM, FL_BAD_SIG, FL_GARBAGE, FL_MAX_SIZE, FL_KINDS };
static const char *const FlashKinds[FL_KINDS] = { "none", "valid", "bad checksum", "bad signature", "garbage",
						    "valid, 255 blocks" };
#define FLASH_ROM_DEVICE 0xf1a5                 /* the PCIR device ID of the flash's ROM: tells which one ran */

static struct {
	int cls, board_vga, flash;
	struct devspec dev[NDEV];
} Spec;
static u8 FlashLow[0x20000];

enum { C_TOPOLOGY, C_CLASS, C_BARS, C_ROMBAR, C_ROMIMG, C_PNP, C_MANY, C_FLASH, C_MIX, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"holes, aliases, bridges, header types", "classes (VGA, display, bridge...)", "BARs ones/zero/odd sizes",
	"ROM BAR size and address", "ROM header, PCIR, checksum, images", "ROM PnP header, BEV/BCV chains",
	"many ROMs (no room left)", "flash boot ROM x NIC x socket ROM", "random fault mix",
};
static const u16 FlashNics[5] = { 0x8139, 0x8039, 0x8169, 0x8168, 0x8029 };
static const unsigned ClassCount[NCLASSES] = { 8 * 2 * 7 * 2, 8 * 4 * 2, 600, 8 * 9 * 2, 2 * 5 * 5 * 2 * 3 * 2 * 3 * 2 * 2,
					       5 * 5 * 6 * 2, 40, FL_KINDS * 5 * 2, 3000 };

static const u8 HeaderTypes[8] = { 0x00, 0x01, 0x02, 0x7f, 0x80, 0x81, 0x82, 0xff };
static const u16 Classes[8] = { 0x0000, 0x0300, 0x0380, 0x0001, 0x0101, 0x0604, 0x0200, 0xffff };
static const u32 Masks[] = { 0, 0xffffffff, 0xfffff800, 0xffff0000, 0xff000000, 0xfff0f000, 0x0000f800, 0x80000000 };
/* The host cannot map the top 8 KiB of the 32-bit space: ROMs stay below; all ones is a read-only BAR. */
static const u32 Bases[] = { 0, 0x000c0000, 0x003ff800, 0x00400000, 0xffe00000, 0xffffffff, 0x7ffff800, 0x80000000,
			     ROM_WINDOW };

static int class_of(unsigned index)
{
	int c = 0;
	while (index >= ClassCount[c])
		index -= ClassCount[c++];
	return c;
}

static unsigned count(void)
{
	unsigned n = 0;
	for (int c = 0; c < NCLASSES; c++)
		n += ClassCount[c];
	return n;
}

/* A plain card in slot 0x0d+i with a valid 16 KiB ROM. */
static struct devspec *card(int i, u16 class)
{
	struct devspec *d = &Spec.dev[i];
	memset(d, 0, sizeof(*d));
	d->present = 1;
	d->slot = 0x0d + i;
	d->vendor = 0x10ec, d->device = 0x8139 + i;
	d->class = class;
	d->rom_mask = 0xffff8001;
	d->rom_base = ROM_WINDOW + i * 0x100000;
	d->has_rom = 1;
	fuzz_rom_default(&d->rom, d->vendor, d->device, 32);
	d->rom.pnp = 1;
	d->rom.bev = 0x80;
	return d;
}

static u16 edge16(void)
{
	static const u16 e[] = { 0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff };
	return fuzz_rand() % 3 ? e[fuzz_rand() % 10] : fuzz_rand();
}

static void build(unsigned index)
{
	int cls = class_of(index);
	unsigned i = index;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(&Spec, 0, sizeof(Spec));
	Spec.cls = cls;
	Spec.board_vga = cls != C_CLASS && fuzz_rand() % 4;
	struct devspec *d = card(0, 0x0200);
	struct fuzz_rom *r = &d->rom;
	switch (cls) {
	case C_TOPOLOGY: {
		static const u8 secondary[7] = { 0, 0, 1, 2, 0xff, 0x10, 1 };
		d->header = HeaderTypes[i % 8];
		d->all_functions = i / 8 % 2;
		if (i / 16 % 7) {                       /* a bridge in slot 0x14 with a card behind it */
			struct devspec *b = card(1, 0x0604), *behind = card(2, 0x0300);
			b->header = 0x01, b->has_rom = 0;
			b->secondary = secondary[i / 16 % 7];
			behind->bus = b->secondary;
			if (i / 16 % 7 == 6) {              /* two bridges claim bus 1 */
				struct devspec *b2 = card(3, 0x0604);
				b2->header = 0x01, b2->secondary = 1, b2->has_rom = 0;
			}
		}
		if (i / 112)
			card(4, 0x0200)->vendor = 0xffff;   /* vendor FFFFh with a ROM: a hole */
		break;
	}
	case C_CLASS:
		d->class = Classes[i % 8];
		d->header = i / 8 % 4 == 3 ? 0x80 : 0;
		if (i / 32)
			card(1, Classes[(i + 1) % 8]);
		break;
	case C_BARS:
		for (int k = 0; k < 6; k++) {
			d->bar_mask[k] = Masks[fuzz_rand() % 8] | (fuzz_rand() % 4 == 0);
			d->bar_val[k] = fuzz_rand() % 2 ? Masks[fuzz_rand() % 8] : fuzz_rand();
		}
		d->rom_mask = Masks[i % 8] | 1;
		break;
	case C_ROMBAR:
		d->rom_mask = Masks[i % 8] | (i / 72 ? 0 : 1);
		d->rom_base = Bases[i / 8 % 9];
		break;
	case C_ROMIMG: {
		static const u8 sizes[5] = { 0, 1, 0x40, 0x80, 0xff };
		static const u16 pcioff[5] = { 0x20, 0, 0x1d, 0xfff0, 0xffff };
		static const u16 ilen[3] = { 32, 0, 0xffff };
		static const u8 types[3] = { 0, 1, 0xff };
		r->sig_bad = i % 2;
		r->size = sizes[i / 2 % 5];
		r->pcioffset = pcioff[i / 10 % 5];
		r->pcir_bad = i / 50 % 2;
		r->ilen = ilen[i / 100 % 3];
		r->indicator = i / 300 % 2 ? 0 : 0x80;
		r->type = types[i / 600 % 3];
		r->images = 1 + i / 1800 % 2;
		r->checksum_bad = i / 3600 % 2;
		if (fuzz_rand() % 4 == 0)
			r->vendor ^= 1;
		d->class = fuzz_rand() % 4 ? 0x0200 : 0x0300;
		break;
	}
	case C_PNP: {
		static const u16 offsets[5] = { 0x40, 0, 0x1f, 0xfff0, 0xffff };
		static const u16 next[6] = { 0, 0x40, 0x60, 0xffff, 0x41, 0x3f };
		r->pnp = 1 + i % 5 / 4;
		r->pnpoffset = offsets[i / 5 % 5];
		r->nextoffset = next[i / 25 % 6];
		r->bev = i / 150 ? 0 : 0x80;
		r->bcv = i / 150 ? 0x90 : 0;
		break;
	}
	case C_FLASH:
		Spec.flash = i % FL_KINDS;
		d->device = FlashNics[i / FL_KINDS % 5];
		d->rom.device = d->device;
		d->has_rom = i / (FL_KINDS * 5);
		break;
	case C_MANY:
		for (int k = 0; k < NDEV; k++) {
			card(k, k == 0 && i % 2 ? 0x0300 : 0x0200)->rom.size = 32 + i * 6;
			Spec.dev[k].rom.ilen = Spec.dev[k].rom.size;
		}
		break;
	default:
		for (int k = 1; k < NDEV; k++)
			if (fuzz_rand() % 2)
				card(k, Classes[fuzz_rand() % 8]);
		for (int k = 0; k < NDEV; k++) {
			struct devspec *x = &Spec.dev[k];
			struct fuzz_rom *y = &x->rom;
			if (!x->present)
				continue;
			for (int f = fuzz_rand() % 4; f > 0; f--)
				switch (fuzz_rand() % 16) {
				case 0: x->header = HeaderTypes[fuzz_rand() % 8]; break;
				case 1: x->all_functions = 1; break;
				case 2: x->rom_mask = Masks[fuzz_rand() % 8] | 1; break;
				case 3: x->rom_base = Bases[fuzz_rand() % 9]; break;
				case 4: y->size = fuzz_rand(); break;
				case 5: y->pcioffset = edge16(); break;
				case 6: y->ilen = edge16(); break;
				case 7: y->indicator = fuzz_rand(); break;
				case 8: y->type = fuzz_rand(); break;
				case 9: y->pnpoffset = edge16(); break;
				case 10: y->nextoffset = edge16(); break;
				case 11: y->images = 2; break;
				case 12: y->sig_bad = 1; break;
				case 13: y->pcir_bad = 1; break;
				case 14: y->checksum_bad = 1; break;
				default: x->secondary = fuzz_rand(), x->header = 1; break;
				}
		}
	}
}

static void describe(unsigned index, char *buf, int size)
{
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "%s, flash ROM %s", Spec.board_vga ? "board VGA" : "no board VGA",
			 FlashKinds[Spec.flash]);
	for (int k = 0; k < NDEV; k++) {
		const struct devspec *d = &Spec.dev[k];
		const struct fuzz_rom *r = &d->rom;
		if (!d->present)
			continue;
		n += snprintf(buf + n, size > n ? size - n : 0, "%s%02x:%02x %04x:%04x class %04x hdr %02x%s sec %02x ROM BAR %08x/%08x",
			      "; ", d->bus, d->slot, d->vendor, d->device, d->class, d->header,
			      d->all_functions ? " all fns" : "", d->secondary, d->rom_base, d->rom_mask);
		if (d->has_rom)
			n += snprintf(buf + n, size > n ? size - n : 0, " ROM sig%s size %u pcir@%x%s %04x:%04x type %u ilen %u ind %02x"
				      " imgs %u%s pnp %u@%x next %x bev %x bcv %x", r->sig_bad ? " bad" : "", r->size,
				      r->pcioffset, r->pcir_bad ? " bad" : "", r->vendor, r->device, r->type, r->ilen,
				      r->indicator, r->images, r->checksum_bad ? " bad sum" : "", r->pnp, r->pnpoffset,
				      r->nextoffset, r->bev, r->bcv);
	}
}

/****************************************************************
 * Run and checks
 ****************************************************************/

static u32 hash(u32 from, u32 to)
{
	u32 h = 2166136261u;
	for (const u8 *p = (void *)from; p < (u8 *)to; p++)
		h = (h ^ *p) * 16777619u;
	return h;
}

static unsigned flash_read(unsigned offset, int size)
{
	unsigned v = 0;
	for (int i = size - 1; i >= 0; i--)
		v = v << 8 | (offset + i < sizeof(FlashLow) ? FlashLow[offset + i] : 0xff);
	return v;
}

static void flash_write(unsigned offset, unsigned value, int size)
{
	fuzz_fail("write to the flash's lower half at %x", offset);
}

static void flash_model(void)
{
	static const struct fuzz_mmio flash = { FLASH_LOW_BASE, sizeof(FlashLow), flash_read, flash_write };
	struct fuzz_rom r;
	memset(FlashLow, 0xff, sizeof(FlashLow));
	fuzz_claim_mmio(&flash);
	if (Spec.flash == FL_NONE)
		return;
	fuzz_rom_default(&r, 0x10ec, FLASH_ROM_DEVICE, Spec.flash == FL_MAX_SIZE ? 255 : 96);
	r.pnp = 1;
	r.bev = 0x80;
	r.sig_bad = Spec.flash == FL_BAD_SIG;
	r.checksum_bad = Spec.flash == FL_BAD_SUM;
	fuzz_rom_build(FlashLow, sizeof(FlashLow), &r);
	if (Spec.flash == FL_GARBAGE)
		for (unsigned k = 0; k < sizeof(FlashLow); k++)
			FlashLow[k] = fuzz_rand();
}

/* The PCIR device ID of the deployed ROM at addr, 0 when it has none. */
static u16 pcir_device(u32 addr)
{
	u16 off = *(u16 *)(addr + 0x18);
	if (off < 0x1a || off > 0xffe0 || memcmp((void *)(addr + off), "PCIR", 4))
		return 0;
	return *(u16 *)(addr + off + 6);
}

static void models_start(void)
{
	flash_model();
	if (Spec.board_vga) {                       /* TGUI9440 in slot 0x0c, 32 KiB ROM */
		struct fuzz_pcidev *vga = fuzz_pci_add(0x60, 0x94401023, 0x03000000, 0);
		struct fuzz_rom r;
		vga->cfg[0x04] = 0x07;
		vga->bar_mask[6] = 0xffff8001;
		vga->cfg[0x33] = 0x17;
		vga->rom_len = 0x8000;
		vga->rom = _malloc(&ZoneHigh, vga->rom_len, 16);
		fuzz_rom_default(&r, 0x1023, 0x9440, 64);
		fuzz_rom_build(vga->rom, vga->rom_len, &r);
	}
	for (int k = 0; k < NDEV; k++) {
		struct devspec *d = &Spec.dev[k];
		if (!d->present)
			continue;
		struct fuzz_pcidev *p = fuzz_pci_add(d->bus << 8 | d->slot << 3 | d->fn, (u32)d->device << 16 | d->vendor,
						     (u32)d->class << 16, d->header);
		p->all_functions = d->all_functions;
		p->cfg[0x04] = 0x07;                    /* I/O, memory, bus master on: coreboot enabled it */
		p->cfg[0x3d] = 1, p->cfg[0x3c] = 11;
		p->cfg[0x19] = d->secondary;
		p->cfg[0x3e] = 0x08;                    /* bridges: VGA forwarding on */
		for (int b = 0; b < 6; b++) {
			p->bar_mask[b] = d->bar_mask[b];
			memcpy(&p->cfg[0x10 + 4 * b], &d->bar_val[b], 4);
		}
		p->bar_mask[6] = d->rom_base == 0xffffffff ? 0 : d->rom_mask;
		u32 rombar = d->rom_base & d->rom_mask & ~1;
		if (!(p->bar_mask[6] & ~1))
			rombar = d->rom_base;               /* BAR reads back a fixed value */
		memcpy(&p->cfg[0x30], &rombar, 4);
		if (d->has_rom) {
			p->rom_len = 0x10000;
			p->rom = _malloc(&ZoneHigh, p->rom_len, 16);
			memset(p->rom, 0xff, p->rom_len);
			fuzz_rom_build(p->rom, p->rom_len, &d->rom);
		}
	}
}

static void check(void)
{
	u32 end = rom_get_last();
	for (int k = 0; k < fuzz_npci; k++)
		if (fuzz_pci[k].cfg[0x30] & fuzz_pci[k].bar_mask[6] & 1)
			fuzz_fail("device %x left its ROM BAR enabled (%08x)", fuzz_pci[k].bdf, fuzz_pci_read(fuzz_pci[k].bdf, 0x30));
	for (int c = 0; c < fuzz_nromcalls; c++) {
		u32 addr = (fuzz_romcalls[c] >> 16) * 16;
		struct rom_header *rom = (void *)addr;
		if (addr < BUILD_ROM_START || addr >= end || rom->signature != OPTION_ROM_SIGNATURE
		    || addr + rom->size * 512 > end)
			fuzz_fail("ran an option ROM at %05x (%04x, %u blocks) outside the deployed ROMs C0000-%05x", addr,
				  rom->signature, rom->size, end);
	}
	if (end > FUZZ_ROM_MAX)
		fuzz_fail("option ROMs deployed up to %05x, beyond %05x", end, FUZZ_ROM_MAX);
	const struct devspec *d = &Spec.dev[0];
	const struct fuzz_rom *r = &d->rom;
	int valid = d->has_rom && !r->sig_bad && r->size && r->size <= 64 && !r->pcir_bad && r->pcioffset == 0x20
		&& !r->type && r->indicator == 0x80 && r->images == 1 && !r->checksum_bad && d->rom_mask == 0xffff8001
		&& d->rom_base == ROM_WINDOW && !d->header && !d->all_functions && r->vendor == d->vendor
		&& r->pnp == 1 && (!r->pnpoffset || r->pnpoffset == 0x40) && !r->nextoffset && Spec.cls != C_MANY;
	if (valid && !fuzz_nromcalls)
		fuzz_fail("a valid PnP option ROM was not run");
	if (fuzz_pci_read(0x90, 0x54) >> 24)
		fuzz_fail("IBC 57h changed to %02x (the flash's lower half decoded)", fuzz_pci_read(0x90, 0x54) >> 24);
	int ran_flash = 0;
	for (int c = 0; c < fuzz_nromcalls; c++)
		ran_flash |= pcir_device((fuzz_romcalls[c] >> 16) * 16) == FLASH_ROM_DEVICE;
	if (ran_flash)
		fuzz_fail("a ROM in the flash's lower half ran for %04x:%04x", d->vendor, d->device);
}

static void run(unsigned index)
{
	build(index);
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	models_start();
	fuzz_pci_model();
	fuzz_setup_init();
	memset(fuzz_isa_image, 0xff, sizeof(fuzz_isa_image));   /* no ISA card: the bus floats */
	for (u32 a = FUZZ_PHYS_LOW; a < FUZZ_PHYS_END; a += 4)
		*(u32 *)a = a * 2654435761u;
	u32 ram = hash(FUZZ_PHYS_LOW, BUILD_ROM_START), bios = hash(BUILD_BIOS_ADDR, 0x100000);
	pci_probe_devices();
	vgarom_setup();
	optionrom_setup();
	fuzz_post_us = fuzz_now_us;
	if (hash(FUZZ_PHYS_LOW, BUILD_ROM_START) != ram)
		fuzz_fail("RAM below C0000 changed during the option ROM scan");
	if (hash(BUILD_BIOS_ADDR, 0x100000) != bios)
		fuzz_fail("BIOS area F0000-FFFFF changed during the option ROM scan");
	check();
	fuzz_setup_screens();
}

const struct fuzz_area fuzz_pcirom = { "pcirom", NCLASSES, ClassNames, count, class_of, describe, run };
