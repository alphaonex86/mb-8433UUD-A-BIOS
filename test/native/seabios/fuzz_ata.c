/* IDE: a two-channel controller (onboard UM8886BF function, legacy ports) whose drives carry the faults,
   run through ata.c detection with setup.c's per-drive options and IdeInfo, block.c's INT 13h geometry,
   reads and writes through the driver, and the POST/setup screens. */
#include "biosvar.h"
#include "block.h"
#include "hw/ata.h"
#include "hw/pcidevice.h"
#include "malloc.h"
#include "newbios_cmos.h"
#include "output.h"
#include "string.h"
#include "stacks.h"
#include "util.h"
#include "devfuzz.h"

enum {
	F_BSY_STUCK = 1 << 0,                       /* BSY from power-on, forever */
	F_BSY_AFTER_CMD = 1 << 1,                   /* BSY forever after the first command */
	F_BSY_AFTER_RESET = 1 << 2,                 /* BSY forever after a soft reset */
	F_DRQ_NEVER = 1 << 3,                       /* data commands end without DRQ */
	F_DRQ_EARLY = 1 << 4,                       /* DRQ before the data is there: what is read early is lost */
	F_DRQ_STUCK = 1 << 5,                       /* DRQ stays set after the last block */
	F_ERR_ALL = 1 << 6,                         /* every command aborts */
	F_ERR_MEDIA = 1 << 7,                       /* reads and writes end with UNC */
	F_REFUSE_SETMULT = 1 << 8,                  /* SET MULTIPLE aborts */
	F_REFUSE_MULT = 1 << 9,                     /* SET MULTIPLE accepted, READ/WRITE MULTIPLE abort */
	F_STATUS_NOISE = 1 << 10,                   /* status register returns random bytes */
	F_ATAPI_NOT_READY = 1 << 11,                /* every packet command: NOT READY */
	F_ATAPI_BSY = 1 << 12,                      /* BSY forever after a packet command */
	F_IGNORE_SRST = 1 << 13,
	F_GHOST = 1 << 14,                          /* ignores the DEV bit: answers as master and slave */
	F_NFAULTS = 15
};
#define F_UNTRUSTED_DATA (F_DRQ_EARLY | F_STATUS_NOISE)   /* the BIOS cannot tell its data is wrong */
#define FOREVER (~0ULL)
#define ST_BSY 0x80
#define ST_RDY 0x40
#define ST_DSC 0x10
#define ST_DRQ 0x08
#define ST_ERR 0x01
#define ER_ABRT 0x04
#define ER_IDNF 0x10
#define ER_UNC 0x40

struct spec_drive {
	u8 present, atapi;
	unsigned faults;
	unsigned reset_us, cmd_us;                  /* BSY time after power-on/reset and after a command */
	u16 id[256];
};

struct spec {
	int cls, healthy_id;
	struct spec_drive drive[4];                 /* primary master, slave, secondary master, slave */
	u8 empty[2], disabled[2];                   /* value read where nobody drives the bus; channel off */
	u8 setting[NB_COUNT];                       /* raw CMOS nibbles, 0xff: default */
	u8 hdd_type[4], hdd_heads[4], hdd_spt[4];
	u16 hdd_cyl[4];
};

static struct spec Spec;

/****************************************************************
 * Drive model
 ****************************************************************/

enum { PH_NONE, PH_IDENTIFY, PH_READ, PH_WRITE, PH_PACKET };

struct drive {
	const struct spec_drive *s;
	u8 tf[8], hob[8], feature, status, err;
	unsigned long long busy_until;
	int phase, multiple, secsize, per_block, pos;
	unsigned long long lba, left;
	u8 cdb[12];
	unsigned long long stamp;
};

struct channel {
	u16 base1, base2;
	u8 dh, empty, disabled;
	struct drive dev[2];
};
static struct channel Chan[2];

static unsigned long long id_capacity(const u16 *id)
{
	if (id[83] & 0x400)
		return id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48;
	if (id[49] & 0x200)
		return id[60] | (u32)id[61] << 16;
	return (u32)id[1] * id[3] * id[6];
}

static void set_busy(struct drive *d, unsigned us, u8 status_after)
{
	d->busy_until = fuzz_now_us + us;
	d->status = status_after;
}

static void reset_drive(struct drive *d, unsigned long long busy)
{
	d->phase = PH_NONE;
	d->busy_until = busy;
	d->status = d->s->atapi ? 0 : ST_RDY | ST_DSC;
	d->err = 1;
	memset(d->tf, 0, sizeof(d->tf));
	d->tf[2] = d->tf[3] = 1;
	if (d->s->atapi)
		d->tf[4] = 0x14, d->tf[5] = 0xeb;
}

static void finish(struct drive *d, u8 err)
{
	d->phase = PH_NONE;
	d->err = err;
	set_busy(d, d->s->cmd_us, ST_RDY | ST_DSC | (err ? ST_ERR : 0));
}

static void start_data(struct drive *d, int phase, unsigned long long lba, unsigned long long count, int per_block,
		       int secsize)
{
	d->phase = phase;
	d->lba = lba;
	d->left = count;
	d->per_block = per_block;
	d->secsize = secsize;
	d->pos = 0;
	if (d->s->faults & F_DRQ_NEVER) {
		finish(d, 0);
		return;
	}
	set_busy(d, phase == PH_WRITE || phase == PH_PACKET ? 1 : d->s->cmd_us, ST_RDY | ST_DSC | ST_DRQ);
}

static int block_bytes(struct drive *d)
{
	return (d->left < (unsigned)d->per_block ? d->left : d->per_block) * d->secsize;
}

static u8 data_byte(struct drive *d, int pos)
{
	if (d->phase == PH_IDENTIFY)
		return d->s->id[pos / 2] >> (pos & 1 ? 8 : 0);
	unsigned long long sector = d->lba + pos / d->secsize;
	int off = pos % d->secsize;
	return off < 8 ? sector >> (8 * off) : sector * 31 + off;
}

static void execute_packet(struct drive *d);

static void block_done(struct drive *d)
{
	int sectors = block_bytes(d) / d->secsize;
	d->lba += sectors;
	d->left -= sectors;
	d->pos = 0;
	if (d->phase == PH_PACKET) {
		execute_packet(d);
	} else if (!d->left) {
		d->phase = PH_NONE;
		set_busy(d, 1, ST_RDY | ST_DSC | (d->s->faults & F_DRQ_STUCK ? ST_DRQ : 0));
	} else {
		set_busy(d, 100, ST_RDY | ST_DSC | ST_DRQ);
	}
}

static int in_data_phase(struct drive *d)
{
	if (d->phase == PH_NONE)
		return 0;
	return fuzz_now_us >= d->busy_until || (d->s->faults & F_DRQ_EARLY);
}

static u8 data_in(struct drive *d)
{
	if (!in_data_phase(d) || d->phase == PH_WRITE || d->phase == PH_PACKET)
		return 0xff;
	u8 v = fuzz_now_us < d->busy_until ? 0xff : data_byte(d, d->pos);
	if (++d->pos == block_bytes(d))
		block_done(d);
	return v;
}

static void data_out(struct drive *d, u8 v)
{
	if (!in_data_phase(d) || (d->phase != PH_WRITE && d->phase != PH_PACKET))
		return;
	if (d->phase == PH_PACKET) {
		d->cdb[d->pos] = v;
	} else {
		int off = d->pos % d->secsize;
		if (!off)
			d->stamp = 0;
		if (off < 8)
			d->stamp |= (unsigned long long)v << (8 * off);
		if (off == 7 && d->stamp != d->lba + d->pos / d->secsize)
			fuzz_fail("write meant for LBA %llu landed on LBA %llu", d->stamp, d->lba + d->pos / d->secsize);
	}
	if (++d->pos == block_bytes(d))
		block_done(d);
}

static void execute_packet(struct drive *d)
{
	const u8 *c = d->cdb;
	if (d->s->faults & F_ATAPI_BSY) {
		d->phase = PH_NONE;
		set_busy(d, 0, ST_BSY), d->busy_until = FOREVER;
	} else if (d->s->faults & F_ATAPI_NOT_READY) {
		finish(d, 0x20 | ER_ABRT);
	} else if (c[0] == 0x28) {
		u32 lba = (u32)c[2] << 24 | c[3] << 16 | c[4] << 8 | c[5], count = c[7] << 8 | c[8];
		if (lba + (u64)count > 333000)
			finish(d, 0x50 | ER_ABRT);          /* illegal request: out of range */
		else if (count)
			start_data(d, PH_READ, lba, count, 1, 2048);
		else
			finish(d, 0);
	} else {
		finish(d, 0x50 | ER_ABRT);
	}
}

static void execute_rw(struct drive *d, u8 cmd)
{
	const u16 *id = d->s->id;
	int ext = cmd == 0x24 || cmd == 0x29 || cmd == 0x34 || cmd == 0x39;
	int mult = cmd == 0xc4 || cmd == 0xc5 || cmd == 0x29 || cmd == 0x39;
	int write = cmd == 0x30 || cmd == 0x34 || cmd == 0xc5 || cmd == 0x39;
	unsigned long long lba, count = ext ? (d->hob[2] << 8 | d->tf[2]) : d->tf[2];
	if (!count)
		count = ext ? 65536 : 256;
	if (d->s->atapi || (ext && !(id[83] & 0x400)))
		return finish(d, ER_ABRT);
	if (d->tf[6] & 0x40) {
		if (!(id[49] & 0x200))
			return finish(d, ER_ABRT);
		lba = ext ? (u64)d->hob[5] << 40 | (u64)d->hob[4] << 32 | (u64)d->hob[3] << 24 | d->tf[5] << 16
			| d->tf[4] << 8 | d->tf[3]
			  : (d->tf[6] & 0xf) << 24 | d->tf[5] << 16 | d->tf[4] << 8 | d->tf[3];
	} else {
		unsigned cyl = d->tf[5] << 8 | d->tf[4], head = d->tf[6] & 0xf, sector = d->tf[3];
		if (!sector || sector > id[6] || head >= id[3] || cyl >= id[1])
			return finish(d, ER_IDNF);
		lba = ((u64)cyl * id[3] + head) * id[6] + sector - 1;
	}
	if (lba + count > id_capacity(id))
		return finish(d, ER_IDNF);
	if (mult && (!d->multiple || (d->s->faults & F_REFUSE_MULT)))
		return finish(d, ER_ABRT);
	if (d->s->faults & F_ERR_MEDIA)
		return finish(d, ER_UNC);
	start_data(d, write ? PH_WRITE : PH_READ, lba, count, mult ? d->multiple : 1, 512);
}

static void execute(struct drive *d, u8 cmd)
{
	const struct spec_drive *s = d->s;
	if (s->faults & F_ERR_ALL)
		return finish(d, ER_ABRT);
	if ((s->faults & F_BSY_AFTER_CMD)) {
		d->phase = PH_NONE;
		d->busy_until = FOREVER;
		return;
	}
	switch (cmd) {
	case 0xec:
	case 0xa1:
		if (s->atapi != (cmd == 0xa1)) {
			finish(d, ER_ABRT);
			if (s->atapi)
				d->tf[4] = 0x14, d->tf[5] = 0xeb;
			return;
		}
		return start_data(d, PH_IDENTIFY, 0, 1, 1, 512);
	case 0xc6: {
		int n = d->tf[2];
		if ((s->faults & F_REFUSE_SETMULT) || s->atapi || !n || n > (s->id[47] & 0xff) || (n & (n - 1)))
			return finish(d, ER_ABRT);
		d->multiple = n;
		return finish(d, 0);
	}
	case 0xef:
	case 0xe3:
	case 0x91:
		return finish(d, 0);
	case 0xa0:
		if (!s->atapi)
			return finish(d, ER_ABRT);
		start_data(d, PH_PACKET, 0, 1, 1, 12);
		d->tf[2] = 1;                           /* CoD: the device wants the command packet */
		return;
	case 0x20: case 0x21: case 0x24: case 0x29: case 0xc4:
	case 0x30: case 0x31: case 0x34: case 0x39: case 0xc5:
		return execute_rw(d, cmd == 0x21 ? 0x20 : cmd == 0x31 ? 0x30 : cmd);
	default:
		return finish(d, ER_ABRT);
	}
}

static u8 status(struct drive *d)
{
	if (d->s->faults & F_STATUS_NOISE)
		return fuzz_rand();
	if (d->s->faults & F_BSY_STUCK)
		return ST_BSY;
	if (fuzz_now_us < d->busy_until)
		return (d->s->faults & F_DRQ_EARLY) && d->phase != PH_NONE ? ST_RDY | ST_DRQ : ST_BSY;
	return d->status;
}

/* Who drives the bus for the selected device: itself, or a ghost answering for both numbers. */
static int channel_off(struct channel *c)
{
	return c->disabled || !(fuzz_pci_read(0x91, 0x40) >> 8 & (c->base1 == 0x1f0 ? 0x80 : 0x40));
}

static struct drive *responder(struct channel *c)
{
	int sel = c->dh >> 4 & 1;
	if (channel_off(c))
		return NULL;
	if (c->dev[sel].s->present)
		return &c->dev[sel];
	if (c->dev[!sel].s->present && (c->dev[!sel].s->faults & F_GHOST))
		return &c->dev[!sel];
	return NULL;
}

static struct channel *channel_of(u16 port)
{
	for (int i = 0; i < 2; i++)
		if ((port & ~7) == Chan[i].base1 || port == Chan[i].base2 + 2)
			return &Chan[i];
	return NULL;
}

static unsigned ata_read(u16 port, int size)
{
	struct channel *c = channel_of(port);
	struct drive *d = responder(c);
	int reg = port == c->base2 + 2 ? 8 : port & 7;
	unsigned empty = c->empty * 0x01010101u;
	if (!d) {
		/* device 0 answers for an absent device 1 with status 0 */
		int sel = c->dh >> 4 & 1;
		if (channel_off(c) || sel == 0 || !c->dev[0].s->present)
			return empty;
		return reg == 0 ? empty : reg == 7 || reg == 8 ? 0 : reg == 1 ? 0 : c->dev[0].tf[reg];
	}
	if (reg == 0) {
		unsigned v = 0;
		for (int i = 0; i < size; i++)
			v |= (unsigned)data_in(d) << (8 * i);
		return v;
	}
	u8 st = status(d);
	if (reg == 7 || reg == 8 || (st & ST_BSY))
		return st;                              /* while BSY every register reads as status */
	return reg == 1 ? d->err : reg == 6 ? (d->tf[6] & ~0x10) | (c->dh & 0x10) : d->tf[reg];
}

static void ata_write(u16 port, unsigned value, int size)
{
	struct channel *c = channel_of(port);
	if (channel_off(c))
		return;
	if (port == c->base2 + 2) {
		for (int i = 0; i < 2; i++)
			if ((value & 4) && !(c->dev[i].s->faults & F_IGNORE_SRST))
				reset_drive(&c->dev[i], c->dev[i].s->faults & F_BSY_AFTER_RESET ? FOREVER
						    : fuzz_now_us + c->dev[i].s->reset_us);
		if (value & 4)
			c->dh = 0;
		return;
	}
	int reg = port & 7;
	struct drive *d = responder(c);
	if (reg == 0) {
		for (int i = 0; d && i < size; i++)
			data_out(d, value >> (8 * i));
		return;
	}
	if (reg == 7) {
		if (d && fuzz_now_us >= d->busy_until)
			execute(d, value);
		return;
	}
	if (reg == 6)
		c->dh = value;
	for (int i = 0; i < 2; i++) {
		struct drive *dev = &c->dev[i];
		if (reg == 1) {
			dev->hob[1] = dev->feature, dev->feature = value;
		} else {
			dev->hob[reg] = dev->tf[reg];
			dev->tf[reg] = value;
		}
	}
}

static void models_start(void)
{
	static const struct fuzz_ports ports[] = {
		{ 0x1f0, 0x1f7, ata_read, ata_write }, { 0x3f6, 0x3f6, ata_read, ata_write },
		{ 0x170, 0x177, ata_read, ata_write }, { 0x376, 0x376, ata_read, ata_write },
	};
	for (int ch = 0; ch < 2; ch++) {
		struct channel *c = &Chan[ch];
		c->base1 = ch ? 0x170 : 0x1f0;
		c->base2 = ch ? 0x374 : 0x3f4;
		c->empty = Spec.empty[ch];
		c->disabled = Spec.disabled[ch];
		for (int i = 0; i < 2; i++) {
			c->dev[i].s = &Spec.drive[ch * 2 + i];
			reset_drive(&c->dev[i], c->dev[i].s->reset_us);
		}
	}
	for (int i = 0; i < 4; i++)
		fuzz_claim(&ports[i]);
}

/****************************************************************
 * Cases
 ****************************************************************/

static const u16 E16[] = { 0, 1, 2, 15, 16, 17, 63, 64, 255, 256, 1024, 0x8000, 0xffff };
static const u32 E32[] = { 0, 1, 2, 63, 0x0fffffff, 0x10000000, 0x7fffffff, 0x80000000, 0xffffffff };
static const u64 E64[] = { 0, 1, 0x0fffffff, 0x10000000, 0xffffffff, 0x100000000ULL, 0xffffffffffffULL,
			   0x1000000000000ULL, 0x8000000000000000ULL, ~0ULL };
static const unsigned Busy[] = { 0, 1000, 400000, 5000000, 31000000, 33000000, 100000000 };
#define N(a) (int)ARRAY_SIZE(a)

static unsigned edge(int bits)
{
	unsigned r = fuzz_rand(), max = bits == 32 ? 0xffffffff : (1u << bits) - 1;
	switch (r % 6) {
	case 0: return r >> 8 & 1 ? 0 : max;
	case 1: return (r >> 8 & 1 ? 1 : max - 1) & max;
	case 2: return 1u << (r >> 8) % bits & max;
	case 3: return ((1u << (r >> 8) % bits) + (r >> 16 & 1 ? 1 : -1)) & max;
	case 4: return (r >> 8 & 1 ? 0x80u : 0x8000u) << (r >> 9 & 1 ? 0 : bits == 32 ? 16 : 0) & max;
	default: return fuzz_rand() & max;
	}
}

static void set_model(u16 *id, const char *name)
{
	for (int i = 0; i < 20; i++) {
		char a = *name ? *name++ : ' ', b = *name ? *name++ : ' ';
		id[27 + i] = (u8)a << 8 | (u8)b;
	}
}

static void set_lba28(u16 *id, u32 n) { id[60] = n, id[61] = n >> 16; }

static void set_lba48(u16 *id, u64 n)
{
	id[83] |= 0x400;
	for (int i = 0; i < 4; i++)
		id[100 + i] = n >> (16 * i);
}

static void healthy_drive(struct spec_drive *d, int atapi)
{
	u16 *id = d->id;
	memset(d, 0, sizeof(*d));
	d->present = 1;
	d->atapi = atapi;
	d->reset_us = 2000;
	d->cmd_us = 200;
	id[0] = atapi ? 0x8580 : 0x0040;
	if (!atapi) {
		id[1] = 16383, id[3] = 16, id[6] = 63;
		id[47] = 0x8010;
		id[49] = 0x0200;
		set_lba28(id, 16383 * 16 * 63);
		id[80] = 0x007e;
		id[83] = 0x4000;
	}
	id[51] = 0x0200, id[53] = 0x0002, id[64] = 0x0003;
	set_model(id, atapi ? "DEVFUZZ CD-ROM" : "DEVFUZZ DISK");
	id[10] = 0x4446, id[11] = 0x3030 + (d - Spec.drive);   /* serial number: "DF0n" */
}

/* Word 93 of device 0 tells whether device 1 answered at reset (SeaBIOS skips device 1 if not). */
static void finish_topology(void)
{
	for (int ch = 0; ch < 2; ch++) {
		struct spec_drive *m = &Spec.drive[ch * 2], *s = &Spec.drive[ch * 2 + 1];
		if (m->present && !m->id[93])
			m->id[93] = s->present ? 0x403b : 0x404b;
		if (s->present && !s->id[93])
			s->id[93] = 0x4b00;
	}
}

enum {
	C_EMPTY, C_GEOMETRY, C_CAPACITY, C_IDWORDS, C_RANDOM_ID, C_BUSY, C_DRQ, C_ERR, C_TOPOLOGY, C_ATAPI,
	C_CMOS, C_MIX, NCLASSES
};
static const char *const ClassNames[NCLASSES] = {
	"no drive, floating bus", "IDENTIFY geometry edges", "IDENTIFY LBA28/LBA48 sizes",
	"IDENTIFY multiple, PIO words, settings", "IDENTIFY random words", "BSY stuck / slow",
	"DRQ never, early, stuck, noisy status", "ERR, refused (SET) MULTIPLE", "master/slave/ghost",
	"ATAPI never ready", "CMOS drive types, user geometry", "random fault mix",
};
static const unsigned ClassCount[NCLASSES] = {
	12, 13 * 13 * 13 * 3, N(E32) * (N(E64) + 1) * 2, 8 * 2 * 4 * 3 * 5, 3000, N(Busy) * N(Busy) * 5 * 2,
	5 * 3 * 2 * 2, 4 * 2 * 3 * 2 * 2, 9 * 2 * 4 * 3, 4 * 3 * 4, 10 * 4 * 4 * 4, 4000,
};

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
	unsigned i = index, r;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(&Spec, 0, sizeof(Spec));
	memset(Spec.setting, 0xff, sizeof(Spec.setting));
	Spec.cls = cls;
	Spec.empty[0] = Spec.empty[1] = 0xff;
	struct spec_drive *pm = &Spec.drive[0], *ps = &Spec.drive[1];
	u16 *id = pm->id;
	switch (cls) {
	case C_EMPTY: {
		static const u8 values[] = { 0xff, 0x00, 0x7f, 0x80, 0x50, 0x01 };
		Spec.empty[0] = Spec.empty[1] = values[i % 6];
		if (i / 6) {
			healthy_drive(pm, 0);
			Spec.setting[NB_IDE_PRIMARY] = 0;   /* drive on a channel switched off in setup */
		}
		break;
	}
	case C_GEOMETRY:
		healthy_drive(pm, 0);
		id[1] = E16[i % 13], id[3] = E16[i / 13 % 13], id[6] = E16[i / 169 % 13];
		r = i / 2197;
		id[49] = r ? 0x200 : 0;
		set_lba28(id, r ? (u32)id[1] * id[3] * id[6] : 0);
		if (r == 2)
			set_lba48(id, (u32)id[1] * id[3] * id[6]);
		break;
	case C_CAPACITY:
		healthy_drive(pm, 0);
		set_lba28(id, E32[i % N(E32)]);
		r = i / N(E32) % (N(E64) + 1);
		if (r)
			set_lba48(id, E64[r - 1]);
		id[49] = i / N(E32) / (N(E64) + 1) ? 0x200 : 0;
		break;
	case C_IDWORDS: {
		static const u16 w47[] = { 0, 1, 2, 16, 0x7f, 0x80, 0xff, 0x8010 }, w51[] = { 0, 0x200, 0x300, 0xff00 };
		static const u16 w53[] = { 0, 2, 0xffff }, w64[] = { 0, 1, 2, 3, 0xffff };
		healthy_drive(pm, 0);
		id[47] = w47[i % 8];
		Spec.setting[NB_IDE_BLOCK] = i / 8 % 2;
		id[51] = w51[i / 16 % 4];
		id[53] = w53[i / 64 % 3];
		id[64] = w64[i / 192 % 5];
		Spec.setting[NB_IDE_PIO0] = fuzz_rand() % 16;
		Spec.setting[NB_IDE_32BIT] = fuzz_rand() % 3;
		Spec.setting[NB_HDD_STANDBY] = fuzz_rand() % 16;
		Spec.setting[NB_IDE_MODE0] = fuzz_rand() % 16;
		id[0] = fuzz_rand() % 4 ? 0x0040 : edge(16);
		id[80] = fuzz_rand() % 4 ? 0x007e : edge(16);
		if (fuzz_rand() % 2)
			for (int w = 27; w < 47; w++)       /* model: control characters, NUL, 8-bit */
				id[w] = edge(16);
		break;
	}
	case C_RANDOM_ID:
		healthy_drive(pm, 0);
		for (int w = 0; w < 256; w++)
			if (fuzz_rand() % 4 == 0)
				id[w] = edge(16);
		if (fuzz_rand() % 2)
			healthy_drive(ps, 0), memcpy(ps->id, id, 512);
		break;
	case C_BUSY: {
		static const unsigned f[] = { 0, F_BSY_STUCK, F_BSY_AFTER_CMD, F_BSY_AFTER_RESET, F_IGNORE_SRST };
		struct spec_drive *d = i / (N(Busy) * N(Busy) * 5) ? ps : pm;
		healthy_drive(d, 0);
		d->reset_us = Busy[i % N(Busy)];
		d->cmd_us = Busy[i / N(Busy) % N(Busy)];
		d->faults = f[i / N(Busy) / N(Busy) % 5];
		break;
	}
	case C_DRQ: {
		static const unsigned f[] = { F_DRQ_NEVER, F_DRQ_EARLY, F_DRQ_STUCK, F_STATUS_NOISE,
					      F_DRQ_EARLY | F_DRQ_STUCK };
		struct spec_drive *d = i / 30 % 2 ? ps : pm;
		healthy_drive(d, i / 60);
		d->faults = f[i % 5];
		d->cmd_us = Busy[i / 5 % 3 * 2];
		break;
	}
	case C_ERR: {
		static const unsigned f[] = { F_ERR_ALL, F_ERR_MEDIA, F_REFUSE_SETMULT, F_REFUSE_MULT };
		static const u16 w47[] = { 0, 0x8010, 0x80ff };
		struct spec_drive *d = i / 48 % 2 ? ps : pm;
		healthy_drive(d, i / 24 % 2);
		d->faults = f[i % 4];
		Spec.setting[NB_IDE_BLOCK] = i / 4 % 2;
		d->id[47] = w47[i / 8 % 3];
		break;
	}
	case C_TOPOLOGY: {
		static const u16 w93[] = { 0, 0xffff, 0x4b4b, 0x0001 };
		int m = i % 3, s = i / 3 % 3;
		if (m)
			healthy_drive(pm, m == 2);
		if (s)
			healthy_drive(ps, s == 2);
		if (i / 9 % 2) {
			if (m)
				pm->faults |= F_GHOST;
			else if (s)
				ps->faults |= F_GHOST;
		}
		if (w93[i / 18 % 4]) {
			pm->id[93] = w93[i / 18 % 4];
			ps->id[93] = w93[i / 18 % 4];
		}
		Spec.empty[0] = (const u8[]){ 0xff, 0x00, 0x7f }[i / 72 % 3];
		break;
	}
	case C_ATAPI: {
		static const unsigned f[] = { 0, F_ATAPI_NOT_READY, F_ATAPI_BSY, F_ERR_ALL };
		static const u16 w0[] = { 0x8580, 0x8500, 0x0000, 0xffff };
		healthy_drive(i / 48 ? ps : pm, 1);
		struct spec_drive *d = pm->present ? pm : ps;
		d->faults = f[i % 4];
		d->cmd_us = Busy[i / 4 % 3 * 2];
		d->id[0] = w0[i / 12 % 4];
		break;
	}
	case C_CMOS: {
		static const u8 types[] = { 0, 1, 2, 16, 17, 47, 48, 49, 0x80, 0xff };
		static const u16 cyl[] = { 0, 1, 1024, 0xffff };
		static const u8 heads[] = { 0, 1, 16, 255 }, spt[] = { 0, 1, 63, 255 };
		int idx = fuzz_rand() % 4;
		Spec.hdd_type[idx] = types[i % 10];
		Spec.hdd_cyl[idx] = cyl[i / 10 % 4];
		Spec.hdd_heads[idx] = heads[i / 40 % 4];
		Spec.hdd_spt[idx] = spt[i / 160 % 4];
		Spec.setting[NB_IDE_MODE0 + idx] = fuzz_rand() % 16;
		r = fuzz_rand() % 3;
		if (r)
			healthy_drive(&Spec.drive[idx], r == 2);
		break;
	}
	default:                                    /* C_MIX */
		for (int d = 0; d < 4; d++) {
			struct spec_drive *s = &Spec.drive[d];
			if (fuzz_rand() % 3 == 0)
				continue;
			healthy_drive(s, fuzz_rand() % 4 == 0);
			for (int k = fuzz_rand() % 3; k > 0; k--)
				s->faults |= 1 << fuzz_rand() % F_NFAULTS;
			s->reset_us = fuzz_rand() % 2 ? Busy[fuzz_rand() % N(Busy)] : fuzz_rand() % 32000000;
			s->cmd_us = fuzz_rand() % 8 ? fuzz_rand() % 5000000 : Busy[fuzz_rand() % N(Busy)];
			for (int k = fuzz_rand() % 6; k > 0; k--)
				s->id[(const u8[]){ 0, 1, 3, 6, 47, 49, 51, 53, 60, 61, 64, 80, 83, 93, 100, 101, 102,
						    103 }[fuzz_rand() % 18]] = edge(16);
		}
		for (int ch = 0; ch < 2; ch++) {
			Spec.empty[ch] = fuzz_rand() % 2 ? 0xff : edge(8);
			Spec.disabled[ch] = fuzz_rand() % 8 == 0;
		}
		for (int k = fuzz_rand() % 4; k > 0; k--)
			Spec.setting[(const u8[]){ NB_IDE_BLOCK, NB_IDE_32BIT, NB_HDD_STANDBY, NB_IDE_PIO0, NB_IDE_PIO1,
						   NB_IDE_PIO2, NB_IDE_PIO3, NB_IDE_MODE0, NB_IDE_MODE1 }[fuzz_rand() % 9]]
				= fuzz_rand() % 16;
		if (fuzz_rand() % 4 == 0) {
			int idx = fuzz_rand() % 4;
			Spec.hdd_type[idx] = fuzz_rand() % 50;
			Spec.hdd_cyl[idx] = edge(16), Spec.hdd_heads[idx] = edge(8), Spec.hdd_spt[idx] = edge(8);
		}
		break;
	}
	finish_topology();
}

static const char *const FaultNames[F_NFAULTS] = {
	"BSY stuck", "BSY after command", "BSY after reset", "DRQ never", "DRQ early", "DRQ stuck", "ERR always",
	"media error", "SET MULTIPLE refused", "READ MULTIPLE refused", "status noise", "ATAPI not ready",
	"ATAPI BSY", "SRST ignored", "ghost (ignores DEV)",
};

static void describe(unsigned index, char *buf, int size)
{
	static const char *const pos[4] = { "pri master", "pri slave", "sec master", "sec slave" };
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "bus %02x/%02x%s%s", Spec.empty[0], Spec.empty[1],
			 Spec.disabled[0] ? " pri off" : "", Spec.disabled[1] ? " sec off" : "");
	for (int d = 0; d < 4; d++) {
		const struct spec_drive *s = &Spec.drive[d];
		if (!s->present)
			continue;
		const u16 *id = s->id;
		n += snprintf(buf + n, size > n ? size - n : 0, "; %s %s busy %u/%u us id0 %04x CHS %u/%u/%u w47 %04x w49 %04x"
			      " lba28 %u w83 %04x lba48 %llu w93 %04x", pos[d], s->atapi ? "ATAPI" : "ATA",
			      s->reset_us, s->cmd_us, id[0], id[1], id[3], id[6], id[47], id[49],
			      id[60] | (u32)id[61] << 16, id[83], id_capacity(id), id[93]);
		for (int f = 0; f < F_NFAULTS; f++)
			if (s->faults & 1 << f)
				n += snprintf(buf + n, size > n ? size - n : 0, ", %s", FaultNames[f]);
	}
	for (int d = 0; d < 4; d++)
		if (Spec.hdd_type[d])
			n += snprintf(buf + n, size > n ? size - n : 0, "; CMOS %s type %u user %u/%u/%u", pos[d], Spec.hdd_type[d],
				      Spec.hdd_cyl[d], Spec.hdd_heads[d], Spec.hdd_spt[d]);
	for (int k = 0; k < NB_COUNT; k++)
		if (Spec.setting[k] != 0xff)
			n += snprintf(buf + n, size > n ? size - n : 0, "; setting %d=%u", k, Spec.setting[k]);
}

/****************************************************************
 * Run and checks
 ****************************************************************/

/* A drive the BIOS must find and use: no fault, in time, and a CD if ATAPI (others do not boot). */
static int healthy(const struct spec_drive *s)
{
	return s->present && !s->faults && s->reset_us <= 31000000 && s->cmd_us <= 31000000
		&& (!s->atapi || (s->id[0] >> 8 & 0x1f) == 5);
}

static void check_detection(void)
{
	int seen[4] = { 0 };
	for (int b = 0; b < fuzz_nboots; b++) {
		struct fuzz_boot *e = &fuzz_boots[b];
		if (e->kind != 'H' && e->kind != 'C')
			continue;
		struct atadrive_s *a = container_of((struct drive_s *)e->drive, struct atadrive_s, drive);
		int pos = a->chan_gf->ataid * 2 + a->slave;
		const struct spec_drive *s = &Spec.drive[pos], *twin = &Spec.drive[pos ^ 1];
		if (pos > 3)
			fuzz_fail("drive registered on ATA channel %d", a->chan_gf->ataid);
		if (seen[pos]++)
			fuzz_fail("ata%d-%d registered twice", pos / 2, pos & 1);
		if (!s->present && twin->present && (twin->faults & F_GHOST))
			continue;                           /* the twin answering for this number */
		if (!s->present)
			fuzz_fail("drive registered at ata%d-%d where there is none", pos / 2, pos & 1);
		if ((e->kind == 'C') != s->atapi)
			fuzz_fail("ata%d-%d: %s registered as %s", pos / 2, pos & 1, s->atapi ? "ATAPI" : "ATA",
				  e->kind == 'C' ? "CD" : "hard disk");
		if (Spec.disabled[pos / 2] || !fuzz_setting(NB_IDE_PRIMARY + pos / 2))
			fuzz_fail("ata%d-%d registered on a channel switched off", pos / 2, pos & 1);
		if (a->drive.sectors == 0 && e->kind == 'H')
			fuzz_fail("ata%d-%d registered with 0 sectors", pos / 2, pos & 1);
	}
	for (int pos = 0; pos < 4; pos += 2) {         /* a ghost: one drive, registered once */
		const struct spec_drive *m = &Spec.drive[pos], *sl = &Spec.drive[pos + 1];
		if (seen[pos] && seen[pos + 1] && m->present != sl->present && (m->faults | sl->faults) & F_GHOST)
			fuzz_fail("ghost: the drive at ata%d-%d also registered as ata%d-%d", pos / 2, m->present ? 0 : 1,
				  pos / 2, m->present ? 1 : 0);
	}
	for (int pos = 0; pos < 4; pos++) {
		struct fuzz_ide info;
		const struct spec_drive *twin = &Spec.drive[pos ^ 1];
		int found = seen[pos] || (!twin->present && (Spec.drive[pos].faults & F_GHOST) && seen[pos ^ 1]);
		fuzz_ideinfo(pos, &info);
		int off = Spec.disabled[pos / 2] || !fuzz_setting(NB_IDE_PRIMARY + pos / 2);
		if (!found && healthy(&Spec.drive[pos]) && Spec.healthy_id && !off
		    && Spec.hdd_type[pos] != NB_HDD_NONE)
			fuzz_fail("working drive at ata%d-%d not detected", pos / 2, pos & 1);
		if (info.present != (unsigned)seen[pos] && !(info.atapi && !seen[pos]))   /* non-CD ATAPI: no boot */
			fuzz_fail("IdeInfo[%d].present=%u but the drive is %sregistered", pos, info.present,
				  seen[pos] ? "" : "not ");
		u64 sectors = seen[pos] ? ((struct drive_s *)info.drive)->sectors : 0;
		if (seen[pos] && !info.atapi && info.sectors != (sectors > 0xffffffff ? 0xffffffff : sectors))
			fuzz_fail("IdeInfo[%d].sectors %u, drive has %llu", pos, info.sectors, sectors);
	}
}

static int transfer(struct drive_s *drive, int command, u64 lba, int count, const struct spec_drive *s)
{
	int bytes = count * drive->blksize;
	fuzz_budget_us = fuzz_now_us + 200000000;   /* one disk access: a few 32 s timeouts at most */
	u8 *buf = _malloc(&ZoneLow, bytes, 4);
	struct disk_op_s op = { .buf_fl = buf, .drive_fl = drive, .command = command, .count = count, .lba = lba };
	for (int k = 0; command == CMD_WRITE && k < count; k++)
		for (int b = 0; b < 8; b++)
			buf[k * drive->blksize + b] = (lba + k) >> (8 * b);
	int ret = drive->type == DTYPE_ATA ? ata_process_op(&op) : ata_atapi_process_op(&op);
	if (!ret && command == CMD_READ && !(s->faults & F_UNTRUSTED_DATA))
		for (int k = 0; k < count; k++) {
			u64 got = 0;
			for (int b = 0; b < 8; b++)
				got |= (u64)buf[k * drive->blksize + b] << (8 * b);
			if (got != lba + k)
				fuzz_fail("read of LBA %llu returned LBA %llu without error", lba + k, got);
		}
	free(buf);
	return ret;
}

static void exercise(void)
{
	for (int b = 0; b < fuzz_nboots; b++) {
		struct fuzz_boot *e = &fuzz_boots[b];
		struct drive_s *drive = e->drive;
		struct atadrive_s *a = container_of(drive, struct atadrive_s, drive);
		int pos = a->chan_gf->ataid * 2 + a->slave;
		const struct spec_drive *s = &Spec.drive[pos];
		if (!s->present)
			s = &Spec.drive[pos ^ 1];               /* a ghost registered under its twin's number */
		if (e->kind == 'C') {
			transfer(drive, CMD_READ, 16, 1, s);
			continue;
		}
		map_hd_drive(drive);
		struct chs_s *l = &drive->lchs;
		if (!l->head || !l->sector || l->sector > 63 || l->head > 255 || l->cylinder > 1024
		    || (!l->cylinder && drive->sectors >= 1008))
			fuzz_fail("ata%d-%d: INT 13h geometry %u/%u/%u (PCHS %u/%u/%u, %llu sectors)", pos / 2, pos & 1,
				  l->cylinder, l->head, l->sector, drive->pchs.cylinder, drive->pchs.head,
				  drive->pchs.sector, drive->sectors);
		u64 last = drive->sectors - 1;
		int must_work = healthy(s) && Spec.healthy_id;
		if (transfer(drive, CMD_READ, 0, 1, s) && must_work)
			fuzz_fail("ata%d-%d: reading LBA 0 of a working drive failed", pos / 2, pos & 1);
		if (drive->sectors >= 4 && transfer(drive, CMD_READ, 0, 3, s) && must_work)
			fuzz_fail("ata%d-%d: reading LBA 0-2 of a working drive failed", pos / 2, pos & 1);
		if (drive->sectors >= 2 && last < (1ULL << 48) && transfer(drive, CMD_READ, last, 1, s) && must_work)
			fuzz_fail("ata%d-%d: reading the last LBA %llu failed", pos / 2, pos & 1, last);
		if (drive->sectors >= 4 && transfer(drive, CMD_WRITE, 1, 2, s) && must_work)
			fuzz_fail("ata%d-%d: writing LBA 1-2 of a working drive failed", pos / 2, pos & 1);
		struct disk_op_s op = { .drive_fl = drive, .command = CMD_RESET };
		ata_process_op(&op);
	}
}

static void run(unsigned index)
{
	build(index);
	Spec.healthy_id = Spec.cls == C_EMPTY || Spec.cls == C_BUSY || Spec.cls == C_TOPOLOGY
		|| Spec.cls == C_ATAPI || Spec.cls == C_CMOS;
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	fuzz_pci_model();
	fuzz_setup_init();                          /* blank CMOS: defaults, then the case's settings */
	for (int k = 0; k < NB_COUNT; k++)
		if (Spec.setting[k] != 0xff)
			fuzz_set_setting(k, Spec.setting[k]);
	for (int d = 0; d < 4; d++)
		fuzz_set_hdd(d, Spec.hdd_type[d], Spec.hdd_cyl[d], Spec.hdd_heads[d], Spec.hdd_spt[d]);
	fuzz_cmos_store();
	fuzz_setup_init();
	models_start();
	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 600000000;   /* a POST stalled for 10 minutes is hung */
	ata_setup();
	wait_threads();
	fuzz_post_us = fuzz_now_us;
	check_detection();
	exercise();
	fuzz_setup_post_warnings();
	fuzz_setup_screens();
	if (index % 64 == 0) {                      /* the disk benchmark: 1 case in 64 (megabytes of PIO reads) */
		fuzz_budget_us = fuzz_now_us + 600000000;
		fuzz_cpu_allow(3000000);
		fuzz_setup_disk_bench();
	}
}

const struct fuzz_area fuzz_ata = { "ata", NCLASSES, ClassNames, count, class_of, describe, run };
