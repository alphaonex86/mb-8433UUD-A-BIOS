/* ISA: option ROMs in C8000-DFFFF (isa_roms_setup() through optionrom_setup()) and ISA PnP cards
   (setup.c setup_isapnp(): initiation key, serial isolation, resource data, configuration), with
   floating buses, garbage ROM headers and lying or endless cards. */
#include "biosvar.h"
#include "hw/pcidevice.h"
#include "malloc.h"
#include "newbios_cmos.h"
#include "output.h"
#include "std/optionrom.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

#define NCARDS 6
#define RES_MAX 512

enum {
	F_ALL_BITS = 1 << 0,                        /* drives 55h/AAh on every isolation read, in any state */
	F_BAD_CHECKSUM = 1 << 1,
	F_NEVER_READY = 1 << 2,                     /* status bit 0 never set */
	F_ENDLESS = 1 << 3,                         /* resource data never ends: FFh forever */
	F_NO_END_TAG = 1 << 4,                      /* small items forever */
	F_IGNORE_WAIT_KEY = 1 << 5,
	F_NFAULTS = 6
};

struct card {
	unsigned faults;
	u8 serial[9];
	u8 res[RES_MAX];
	int nres;
	/* state */
	int state, keypos, csn, bit, pos, ldn, stay;
	u8 regs[8][256];
};
enum { WAIT_KEY, SLEEP, ISOLATION, CONFIG };

static struct {
	int cls, ncards, isapnp_on, no_vga;
	u8 bus;                                     /* read port value when nobody drives it */
	struct card card[NCARDS];
	struct { u32 at; struct fuzz_rom rom; } isarom[6];
	int nroms;
	u8 isa_fill;
} Spec;

/****************************************************************
 * ISA PnP card model (ISA Plug and Play 1.0a)
 ****************************************************************/

static u8 Address, Key[32];
static u16 ReadPort;
static int PairByte;

static void serial_checksum(u8 *serial, int bad)
{
	u8 sum = 0x6a;
	for (int i = 0; i < 64; i++) {
		int bit = serial[i / 8] >> (i % 8) & 1;
		sum = (((sum ^ sum >> 1) & 1) ^ bit) << 7 | sum >> 1;
	}
	serial[8] = sum ^ (bad ? 0x5a : 0);
}

static u8 res_byte(struct card *c)
{
	int p = c->pos++;
	if (p < 9)
		return c->serial[p];
	if (c->faults & F_ENDLESS)
		return 0xff;
	if (p - 9 < c->nres)
		return c->res[p - 9];
	return c->faults & F_NO_END_TAG ? 0x22 : 0x79;   /* IRQ items forever, or the end tag */
}

static unsigned pnp_read(u16 port, int size)
{
	if (port != ReadPort || !ReadPort)
		return Spec.bus;
	int driven = 0;
	u8 v = Spec.bus;
	if (Address == 0x01) {                      /* isolation: cards with a 1 bit drive 55h then AAh */
		int any = 0;
		for (int i = 0; i < Spec.ncards; i++) {
			struct card *c = &Spec.card[i];
			if ((c->state == ISOLATION && c->bit < 72 && (c->serial[c->bit / 8] >> (c->bit % 8) & 1))
			    || (c->faults & F_ALL_BITS))
				any = 1;
		}
		v = any ? (PairByte ? 0xaa : 0x55) : Spec.bus;
		if (PairByte)
			for (int i = 0; i < Spec.ncards; i++) {
				struct card *c = &Spec.card[i];
				if (c->state != ISOLATION || c->bit >= 72)
					continue;
				if (any && !(c->serial[c->bit / 8] >> (c->bit % 8) & 1))
					c->state = SLEEP;       /* lost: another card drove its 1 */
				c->bit++;
			}
		PairByte ^= 1;
		return v;
	}
	for (int i = 0; i < Spec.ncards; i++) {
		struct card *c = &Spec.card[i];
		if (c->state != CONFIG)
			continue;
		if (Address == 0x05)
			v = (c->faults & F_NEVER_READY) ? 0 : 1;
		else if (Address == 0x04)
			v = res_byte(c);
		else if (Address == 0x06)
			v = c->csn;
		else
			v = c->regs[c->ldn & 7][Address];
		driven++;
	}
	return driven ? v : Spec.bus;
}

static void key_write(struct card *c, u8 v)
{
	if (c->state != WAIT_KEY)
		return;
	c->keypos = v == Key[c->keypos] ? c->keypos + 1 : v == Key[0];
	if (c->keypos == 32)
		c->state = SLEEP, c->keypos = 0;
}

static void data_write(struct card *c, u8 v)
{
	switch (Address) {
	case 0x00:
		ReadPort = v << 2 | 3;
		break;
	case 0x02:
		if (v & 4)
			c->csn = 0;
		if ((v & 2) && !(c->faults & F_IGNORE_WAIT_KEY))
			c->state = WAIT_KEY;
		break;
	case 0x03:
		if (c->state == WAIT_KEY)
			break;
		if (v == c->csn) {
			c->state = v ? CONFIG : ISOLATION;
			c->bit = c->pos = 0;
		} else {
			c->state = SLEEP;
		}
		break;
	case 0x06:
		if (c->state == ISOLATION && c->bit >= 72)
			c->csn = v, c->state = CONFIG, c->pos = 0;
		break;
	case 0x07:
		if (c->state == CONFIG)
			c->ldn = v;
		break;
	default:
		if (c->state == CONFIG && Address >= 0x30)
			c->regs[c->ldn & 7][Address] = v;
	}
}

static void pnp_write(u16 port, unsigned value, int size)
{
	if (port == 0x279) {
		Address = value;
		PairByte = 0;
		for (int i = 0; i < Spec.ncards; i++)
			key_write(&Spec.card[i], value);
	} else if (port == 0xa79) {
		for (int i = 0; i < Spec.ncards; i++)
			if (Spec.card[i].state != WAIT_KEY)
				data_write(&Spec.card[i], value);
	}
}

static void pnp_start(void)
{
	static const struct fuzz_ports ports[] = { { 0x279, 0x279, pnp_read, pnp_write },
						   { 0xa79, 0xa79, pnp_read, pnp_write },
						   { 0x203, 0x3ff, pnp_read, pnp_write } };
	u8 v = 0x6a;
	for (int i = 0; i < 32; i++, v = v >> 1 | ((v ^ v >> 1) & 1) << 7)
		Key[i] = v;
	for (int i = 0; i < 3; i++)
		fuzz_claim(&ports[i]);
}

/****************************************************************
 * Cases
 ****************************************************************/

static int add(struct card *c, const u8 *bytes, int n)
{
	if (c->nres + n > RES_MAX)
		return 0;
	memcpy(c->res + c->nres, bytes, n);
	c->nres += n;
	return n;
}

/* A sound-card-like device: one logical device with I/O, IRQ and DMA, plus a dependent choice. */
static void good_card(struct card *c, int n)
{
	memset(c, 0, sizeof(*c));
	u8 serial[8] = { 0x0e, 0x8c, 0x00, 0x28 + n, 0x12, 0x34, 0x56, n };
	memcpy(c->serial, serial, 8);
	serial_checksum(c->serial, 0);
	add(c, (const u8[]){ 0x0a, 0x10, 0x10 }, 3);                       /* PnP version */
	add(c, (const u8[]){ 0x82, 0x07, 0x00, 'D', 'E', 'V', 'F', 'U', 'Z', 'Z' }, 10);
	add(c, (const u8[]){ 0x15, 0x0e, 0x8c, 0x00, 0x01, 0x00 }, 6);     /* logical device */
	add(c, (const u8[]){ 0x47, 0x01, 0x20, 0x02, 0x80, 0x02, 0x20, 0x10 }, 8);
	add(c, (const u8[]){ 0x22, 0xa0, 0x00 }, 3);                       /* IRQ 5 or 7 */
	add(c, (const u8[]){ 0x2a, 0x02, 0x00 }, 3);                       /* DMA 1 */
	add(c, (const u8[]){ 0x30, 0x47, 0x01, 0x30, 0x03, 0x30, 0x03, 0x08, 0x08, 0x38 }, 10);
}

static void end_tag(struct card *c)
{
	add(c, (const u8[]){ 0x79, 0x00 }, 2);
}

static const u8 E8[] = { 0x00, 0x01, 0x7f, 0x80, 0xff, 0xfe, 0x10, 0x0f };

/* A descriptor whose every byte is an edge value, of any small/large item kind and length. */
static void edge_item(struct card *c)
{
	static const u8 tags[] = { 0x20, 0x28, 0x40, 0x48, 0x30, 0x38, 0x10, 0x78, 0x08, 0x70 };
	u8 item[12];
	int small = fuzz_rand() % 4, len = fuzz_rand() % 8;
	if (small) {
		item[0] = tags[fuzz_rand() % 10] | len;
		for (int k = 1; k <= len; k++)
			item[k] = fuzz_rand() % 2 ? E8[fuzz_rand() % 8] : fuzz_rand();
		add(c, item, 1 + len);
	} else {
		static const u8 large[] = { 0x81, 0x82, 0x84, 0x85, 0x86, 0xff };
		item[0] = large[fuzz_rand() % 6];
		item[1] = len, item[2] = fuzz_rand() % 8 == 0 ? 0xff : 0;
		for (int k = 3; k < 3 + len; k++)
			item[k] = E8[fuzz_rand() % 8];
		add(c, item, 3 + len);
	}
}

static void isa_rom_default(int k, u32 at, u8 blocks)
{
	Spec.isarom[k].at = at;
	fuzz_rom_default(&Spec.isarom[k].rom, 0, 0, blocks);
	Spec.isarom[k].rom.pcioffset = 0;
	Spec.nroms = k + 1;
}

enum { C_PNP_BUS, C_PNP_CARDS, C_PNP_EDGES, C_PNP_FAULTS, C_ROM_HEADERS, C_ROM_LAYOUT, C_ROM_PNP, C_MIX, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"PnP: no card, floating bus", "PnP: 1-6 cards, same serial", "PnP: resource descriptor edges",
	"PnP: all bits, bad checksum, endless", "ISA ROM: header size/checksum", "ISA ROM: placement, overlap, no VGA",
	"ISA ROM: PnP header, BEV/BCV chains", "random fault mix",
};
static const unsigned ClassCount[NCLASSES] = { 8 * 2, 6 * 2 * 2, 2000, 6 * 4 * 2, 8 * 2 * 2 * 3 * 2, 6 * 8 * 2, 5 * 6 * 2 * 2, 2000 };

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

static void build(unsigned index)
{
	int cls = class_of(index);
	unsigned i = index;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(&Spec, 0, sizeof(Spec));
	Spec.cls = cls;
	Spec.isapnp_on = 1;
	Spec.bus = 0xff;
	Spec.isa_fill = 0xff;
	switch (cls) {
	case C_PNP_BUS:
		Spec.bus = E8[i % 8];
		Spec.isapnp_on = i / 8;
		break;
	case C_PNP_CARDS:
		Spec.ncards = 1 + i % 6;
		for (int k = 0; k < Spec.ncards; k++) {
			good_card(&Spec.card[k], i / 6 % 2 ? 0 : k);   /* identical serials: they win together */
			end_tag(&Spec.card[k]);
		}
		Spec.isapnp_on = i / 12 % 2;
		break;
	case C_PNP_EDGES:
		Spec.ncards = 1 + fuzz_rand() % 2;
		for (int k = 0; k < Spec.ncards; k++) {
			good_card(&Spec.card[k], k);
			for (int e = 1 + fuzz_rand() % 8; e > 0; e--)
				edge_item(&Spec.card[k]);
			end_tag(&Spec.card[k]);
		}
		break;
	case C_PNP_FAULTS: {
		static const unsigned f[6] = { F_ALL_BITS, F_BAD_CHECKSUM, F_NEVER_READY, F_ENDLESS, F_NO_END_TAG,
					       F_IGNORE_WAIT_KEY };
		Spec.ncards = 1 + i / 6 % 4;
		for (int k = 0; k < Spec.ncards; k++) {
			good_card(&Spec.card[k], k);
			end_tag(&Spec.card[k]);
		}
		Spec.card[0].faults = f[i % 6];
		serial_checksum(Spec.card[0].serial, f[i % 6] == F_BAD_CHECKSUM);
		Spec.bus = i / 24 ? 0x00 : 0xff;
		break;
	}
	case C_ROM_HEADERS: {
		static const u8 sizes[8] = { 0, 1, 2, 0x10, 0x40, 0x7f, 0x80, 0xff };
		isa_rom_default(0, 0xc8000, sizes[i % 8]);
		Spec.isarom[0].rom.checksum_bad = i / 8 % 2;
		Spec.isarom[0].rom.sig_bad = i / 16 % 2;
		Spec.isarom[0].rom.pnp = i / 32 % 3 ? 1 : 0;
		Spec.isarom[0].rom.bcv = i / 32 % 3 == 2 ? 0x10 : 0;
		Spec.isarom[0].rom.bev = i / 32 % 3 == 1 ? 0x10 : 0;
		Spec.no_vga = i / 96;
		break;
	}
	case C_ROM_LAYOUT: {
		static const u32 at[6] = { 0xc8000, 0xc8800, 0xd0000, 0xdf800, 0xdc000, 0xde000 };
		static const u8 sizes[8] = { 0x10, 0x20, 0x40, 0x7f, 0x80, 0xff, 0x04, 0x01 };
		isa_rom_default(0, at[i % 6], sizes[i / 6 % 8]);
		isa_rom_default(1, at[i % 6] + 0x800, 4);          /* overlaps the first one */
		Spec.no_vga = i / 48;
		Spec.isa_fill = i / 48 ? 0x00 : 0xff;
		break;
	}
	case C_ROM_PNP: {
		static const u16 off[5] = { 0x40, 0, 0x3f, 0xfff0, 0xffff };
		static const u16 next[6] = { 0, 0x40, 0x60, 0xffff, 0x41, 0x3f };
		isa_rom_default(0, 0xc8000, 0x10);
		struct fuzz_rom *r = &Spec.isarom[0].rom;
		r->pnp = 1;
		r->pnpoffset = off[i % 5];
		r->nextoffset = next[i / 5 % 6];
		r->bev = i / 30 % 2 ? 0x10 : 0;
		r->bcv = i / 60 % 2 ? 0x20 : 0;
		break;
	}
	default:
		Spec.bus = fuzz_rand() % 2 ? 0xff : E8[fuzz_rand() % 8];
		Spec.ncards = fuzz_rand() % 4;
		for (int k = 0; k < Spec.ncards; k++) {
			good_card(&Spec.card[k], fuzz_rand() % 3);
			for (int e = fuzz_rand() % 4; e > 0; e--)
				edge_item(&Spec.card[k]);
			if (fuzz_rand() % 8)
				end_tag(&Spec.card[k]);
			if (fuzz_rand() % 4 == 0)
				Spec.card[k].faults = 1 << fuzz_rand() % F_NFAULTS;
			serial_checksum(Spec.card[k].serial, Spec.card[k].faults & F_BAD_CHECKSUM);
		}
		for (int k = fuzz_rand() % 4; k > 0; k--) {
			isa_rom_default(Spec.nroms, 0xc8000 + (fuzz_rand() % 48) * 0x800, fuzz_rand() % 2 ? fuzz_rand() : 0x10);
			struct fuzz_rom *r = &Spec.isarom[Spec.nroms - 1].rom;
			r->checksum_bad = fuzz_rand() % 4 == 0;
			r->pnp = fuzz_rand() % 3;
			r->pnpoffset = fuzz_rand() % 2 ? 0 : fuzz_rand();
			r->nextoffset = fuzz_rand() % 2 ? 0 : fuzz_rand() % 3 ? 0x40 : fuzz_rand();
			r->bev = fuzz_rand() % 2 ? 0x10 : 0;
			r->bcv = fuzz_rand() % 2 ? 0x20 : 0;
		}
		Spec.no_vga = fuzz_rand() % 4 == 0;
	}
}

static const char *const FaultNames[F_NFAULTS] = {
	"answers every isolation bit", "bad serial checksum", "never ready", "endless FFh resource data",
	"no end tag", "ignores Wait for Key",
};

static void describe(unsigned index, char *buf, int size)
{
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "PnP %s, bus %02x, %s VGA ROM, ISA fill %02x", Spec.isapnp_on ? "on" : "off",
			 Spec.bus, Spec.no_vga ? "no" : "PCI", Spec.isa_fill);
	for (int k = 0; k < Spec.ncards; k++) {
		struct card *c = &Spec.card[k];
		n += snprintf(buf + n, size > n ? size - n : 0, "; card %d serial %02x%02x%02x%02x.%02x%02x%02x%02x sum %02x res", k,
			      c->serial[0], c->serial[1], c->serial[2], c->serial[3], c->serial[4], c->serial[5],
			      c->serial[6], c->serial[7], c->serial[8]);
		for (int b = 0; b < c->nres && n < size - 4; b++)
			n += snprintf(buf + n, size > n ? size - n : 0, " %02x", c->res[b]);
		for (int f = 0; f < F_NFAULTS; f++)
			if (c->faults & 1 << f)
				n += snprintf(buf + n, size > n ? size - n : 0, ", %s", FaultNames[f]);
	}
	for (int k = 0; k < Spec.nroms; k++) {
		const struct fuzz_rom *r = &Spec.isarom[k].rom;
		n += snprintf(buf + n, size > n ? size - n : 0, "; ROM at %05x size %u%s%s pnp %u@%x next %x bev %x bcv %x",
			      Spec.isarom[k].at, r->size, r->sig_bad ? " bad sig" : "", r->checksum_bad ? " bad sum" : "",
			      r->pnp, r->pnpoffset, r->nextoffset, r->bev, r->bcv);
	}
}

/****************************************************************
 * Run and checks
 ****************************************************************/

static void check_pnp(void)
{
	static const u8 taken_irqs[] = { 0, 1, 2, 3, 4, 6, 7, 8, 13, 14, 15 };
	u16 io_used[64][2];
	int nio = 0;
	for (int k = 0; k < Spec.ncards; k++) {
		struct card *c = &Spec.card[k];
		if (c->state != WAIT_KEY && !(c->faults & F_IGNORE_WAIT_KEY))
			fuzz_fail("card %d left out of Wait for Key (state %d)", k, c->state);
		for (int l = 0; l < 8; l++) {
			u8 *r = c->regs[l];
			if (!r[0x30])
				continue;
			for (int q = 0; q < 2; q++)
				for (int t = 0; t < (int)sizeof(taken_irqs); t++)
					if (r[0x70 + 2 * q] && r[0x70 + 2 * q] == taken_irqs[t])
						fuzz_fail("card %d LDN %d given IRQ %d, used by the board", k, l, taken_irqs[t]);
			for (int q = 0; q < 2; q++)
				if (r[0x74 + q] == 2)
					fuzz_fail("card %d LDN %d given DMA %d", k, l, r[0x74 + q]);
			for (int q = 0; q < 8 && nio < 64; q++) {
				u16 base = r[0x60 + 2 * q] << 8 | r[0x61 + 2 * q];
				if (!base)
					continue;
				if (base < 0x100 || base > 0x3ff)
					fuzz_fail("card %d LDN %d given I/O %x outside 100-3FF", k, l, base);
				for (int u = 0; u < nio; u++)       /* same serial: one card to the BIOS, it cannot tell */
					if (io_used[u][0] == base && io_used[u][1] != k
					    && memcmp(Spec.card[io_used[u][1]].serial, c->serial, 9))
						fuzz_fail("cards %d and %d both given I/O %x", io_used[u][1], k, base);
				io_used[nio][0] = base, io_used[nio++][1] = k;
			}
		}
	}
}

static u32 hash(u32 from, u32 to)
{
	u32 h = 2166136261u;
	for (const u8 *p = (void *)from; p < (u8 *)to; p++)
		h = (h ^ *p) * 16777619u;
	return h;
}

static void check_roms(void)
{
	u32 end = rom_get_last();
	for (int c = 0; c < fuzz_nromcalls; c++) {
		u32 addr = (fuzz_romcalls[c] >> 16) * 16;
		struct rom_header *rom = (void *)addr;
		if (addr < BUILD_ROM_START || addr >= end || rom->signature != OPTION_ROM_SIGNATURE)
			fuzz_fail("ran an option ROM at %05x (%04x) outside the deployed ROMs C0000-%05x", addr,
				  rom->signature, end);
	}
	for (int k = 0; k < Spec.nroms; k++) {
		const struct fuzz_rom *r = &Spec.isarom[k].rom;
		u32 at = Spec.isarom[k].at;
		int run = 0;
		for (int c = 0; c < fuzz_nromcalls; c++)
			run += (fuzz_romcalls[c] >> 16) * 16 == at;
		for (int b = 0; b < fuzz_nboots; b++)
			run += fuzz_boots[b].seg * 16 == at && (fuzz_boots[b].kind == 'V' || fuzz_boots[b].kind == 'E');
		int overlap = 0;
		for (int o = 0; o < Spec.nroms; o++)
			overlap |= o != k && Spec.isarom[o].at < at + (r->size ?: 1) * 512
				&& at < Spec.isarom[o].at + (Spec.isarom[o].rom.size ?: 1) * 512;
		if (run && !overlap && (r->checksum_bad || r->sig_bad || !r->size))
			fuzz_fail("ISA ROM at %05x with a bad %s used", at, r->size ? "checksum" : "size");
		if (!run && k == 0 && Spec.nroms == 1 && !r->checksum_bad && !r->sig_bad && r->size
		    && at + r->size * 512 <= 0xe0000 && (!r->pnp || r->bev || r->bcv) && !Spec.no_vga)
			fuzz_fail("valid ISA ROM at %05x neither run nor registered", at);
	}
}

static void run(unsigned index)
{
	build(index);
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	if (!Spec.no_vga) {                         /* PCI VGA with a 32 KiB ROM: ISA ROMs start at C8000 */
		struct fuzz_pcidev *vga = fuzz_pci_add(0x68, 0x94401023, 0x03000000, 0);
		struct fuzz_rom r;
		vga->cfg[0x04] = 0x07;
		vga->bar_mask[6] = 0xffff8001;
		vga->cfg[0x33] = 0x18;
		vga->rom_len = 0x8000;
		vga->rom = _malloc(&ZoneHigh, vga->rom_len, 16);
		fuzz_rom_default(&r, 0x1023, 0x9440, 64);
		fuzz_rom_build(vga->rom, vga->rom_len, &r);
	}
	fuzz_pci_model();
	pnp_start();
	fuzz_setup_init();
	fuzz_set_setting(NB_ISAPNP, Spec.isapnp_on);
	memset(fuzz_isa_image, Spec.isa_fill, sizeof(fuzz_isa_image));
	for (int k = 0; k < Spec.nroms; k++) {
		u32 at = Spec.isarom[k].at;
		fuzz_rom_build(&fuzz_isa_image[at - 0xc8000], 0xe0000 - at, &Spec.isarom[k].rom);
	}
	for (u32 a = FUZZ_PHYS_LOW; a < BUILD_ROM_START; a += 4)
		*(u32 *)a = a * 2654435761u;
	u32 ram = hash(FUZZ_PHYS_LOW, BUILD_ROM_START), bios = hash(BUILD_BIOS_ADDR, 0x100000);

	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 30000000;    /* PnP isolation and configuration: 30 s is a hang */
	fuzz_setup_isapnp();
	fuzz_budget_us = fuzz_now_us + 60000000;
	vgarom_setup();
	optionrom_setup();
	fuzz_post_us = fuzz_now_us;
	if (hash(FUZZ_PHYS_LOW, BUILD_ROM_START) != ram)
		fuzz_fail("RAM below C0000 changed during ISA setup");
	if (hash(BUILD_BIOS_ADDR, 0x100000) != bios)
		fuzz_fail("BIOS area F0000-FFFFF changed during ISA setup");
	if (Spec.isapnp_on)
		check_pnp();
	check_roms();
	fuzz_setup_pnp();                           /* their ESCD records (no flash chip: not written) */
	fuzz_setup_screens();
}

const struct fuzz_area fuzz_isa = { "isa", NCLASSES, ClassNames, count, class_of, describe, run };
