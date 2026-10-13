/* PnP BIOS (pnpbios.c): the POST's system device nodes and ESCD with every flash chip (Intel 28F001BX-T,
   Winbond W29C010, unknown) and fault (Vpp low, status stuck busy, erase/program error, page never
   written, wrong ID, refresh bit stuck), a corrupt or other ESCD in the flash, a second POST that must not
   write again, then every runtime function with edge and garbage arguments. */
#include "biosvar.h"
#include "malloc.h"
#include "output.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

int fork(void);
int waitpid(int pid, int *status, int options);
void _exit(int status);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);

u16 handle_pnp(u16 *args, int prot);
u32 escd_rw32(int write, u32 segoff);
u32 _cfunc32flat_escd_rw32(u32 write, u32 segoff, u32 unused)    /* call32_params() of real mode */
{
	return escd_rw32(write, segoff);
}
extern u8 PnpNodes[768], PnpNodeCount, FlashChip;
extern u16 PnpNodeMax;

#define ESCD 0xc000                             /* the ESCD block in the 64 KiB window (flash 0x1c000) */
#define BUF 0x40000                             /* caller buffers (physical) */

enum { INTEL, W29, OTHER, NCHIPS };
enum {
	F_VPP = 1 << 0,                             /* Intel: Vpp low (JP13 at 5 V) */
	F_BUSY = 1 << 1,                            /* Intel: status bit 7 never set */
	F_ERASE_ERR = 1 << 2,
	F_PROG_ERR = 1 << 3,
	F_NEVER = 1 << 4,                           /* W29: page write never ends */
	F_BAD_ID = 1 << 5,
	F_NO_REFRESH = 1 << 6,                      /* port 61h bit 4 does not toggle */
	F_NFAULTS = 7
};
enum { ARRAY, STATUS, ID, ERASE_SETUP, PROGRAM_SETUP, LOAD };

static struct {
	int cls, chip, faults, content, csns;
	u8 board, mouse, lpt_mode;
	u16 com[4], lpt;
	u32 ram;
	int nldev;
	struct { u8 csn, id[8], iolen[8], irq[2], dma[2]; u16 io[8]; } ldev[24];
} Spec;

static u8 *Flash;                               /* the 64 KiB window, shared with a first boot's process */
static u8 Status, Refresh;
static int Mode, Cycle, Page = -1, Writes, Erases, Programs;
static u8 PageBuf[128], PageSet[128];

/****************************************************************
 * Flash chip and port 61h models
 ****************************************************************/

static void outside(unsigned off, const char *what)
{
	if (off < ESCD || off >= ESCD + 0x1000)
		fuzz_fail("%s at flash %05x, outside the ESCD block", what, 0x10000 + off);
}

static void w29_commit(void)
{
	if (Page < 0 || (Spec.faults & F_NEVER))
		return;
	for (int i = 0; i < 128; i++)
		Flash[Page + i] = PageSet[i] ? PageBuf[i] : 0xff;
	Programs++;
	Page = -1;
	Mode = ARRAY;
}

static unsigned flash_read(unsigned off, int size)
{
	if (size != 1)
		fuzz_fail("flash read of %d bytes", size);
	if (Spec.chip == INTEL && Mode == STATUS)
		return Status;
	if (Mode == ID)
		return Spec.faults & F_BAD_ID ? 0x5a : (off & 1) ? (Spec.chip == INTEL ? 0x94 : 0xc1)
								    : (Spec.chip == INTEL ? 0x89 : 0xda);
	if (Mode == LOAD) {
		if (Spec.faults & F_NEVER)
			return Flash[off] ^ 0x80;           /* data polling: bit 7 inverted while busy */
		w29_commit();
	}
	return Flash[off];
}

static void intel_write(unsigned off, u8 val)
{
	if (Mode == ERASE_SETUP) {
		Mode = STATUS;
		if (val != 0xd0) {
			Status |= 0x30;
			return;
		}
		outside(off, "block erase");
		Erases++;
		Status = Spec.faults & F_BUSY ? 0 : Spec.faults & F_VPP ? 0xa8 : Spec.faults & F_ERASE_ERR ? 0xa0 : 0x80;
		if (Status == 0x80)
			memset(&Flash[ESCD], 0xff, 0x1000);
		return;
	}
	if (Mode == PROGRAM_SETUP) {
		outside(off, "byte program");
		Mode = STATUS;
		Programs++;
		Status = Spec.faults & F_BUSY ? 0 : Spec.faults & F_VPP ? 0x98 : Spec.faults & F_PROG_ERR ? 0x90 : 0x80;
		if (Status == 0x80)
			Flash[off] &= val;
		return;
	}
	if (Spec.faults & F_BUSY && Status == 0)
		return;                                 /* still busy: commands ignored */
	switch (val) {
	case 0xff: Mode = ARRAY; break;
	case 0x70: Mode = STATUS; break;
	case 0x50: Status &= 0x80; break;
	case 0x90: Mode = ID; break;
	case 0x20: Mode = ERASE_SETUP; break;
	case 0x40: case 0x10: Mode = PROGRAM_SETUP; break;
	default: break;
	}
}

static void w29_write(unsigned off, u8 val)
{
	static const u16 addr[3] = { 0x5555, 0x2aaa, 0x5555 };
	static const u8 data[2] = { 0xaa, 0x55 };
	if (Mode == LOAD && Cycle == 0 && !(off == 0x5555 && val == 0xaa)) {
		if (Page >= 0 && (off & ~127) != (unsigned)Page)
			fuzz_fail("page load at %05x into the page of %05x", 0x10000 + off, 0x10000 + Page);
		outside(off, "page write");
		Page = off & ~127;
		PageBuf[off & 127] = val;
		PageSet[off & 127] = 1;
		return;
	}
	if ((off & 0x7fff) != addr[Cycle] || (Cycle < 2 && val != data[Cycle])) {
		Cycle = 0;                              /* software data protection: other writes ignored */
		return;
	}
	if (Cycle++ < 2)
		return;
	Cycle = 0;
	w29_commit();
	if (val == 0x90)
		Mode = ID;
	else if (val == 0xf0)
		Mode = ARRAY;
	else if (val == 0xa0) {
		Mode = LOAD;
		Page = -1;
		memset(PageSet, 0, sizeof(PageSet));
	}
}

static void flash_write(unsigned off, unsigned val, int size)
{
	if (size != 1)
		fuzz_fail("flash write of %d bytes", size);
	Writes++;
	if (Spec.chip == INTEL)
		intel_write(off, val);
	else if (Spec.chip == W29)
		w29_write(off, val);
}

static unsigned port61_read(unsigned short port, int size)
{
	if (!(Spec.faults & F_NO_REFRESH))
		Refresh ^= 0x10;
	return Refresh | 0x20;
}

static void port61_write(unsigned short port, unsigned value, int size) { }

static void board_start(void)
{
	static const struct fuzz_ports port61 = { 0x61, 0x61, port61_read, port61_write };
	static const struct fuzz_mmio flash = { PNP_FLASH_BASE, 0x10000, flash_read, flash_write };
	fuzz_bios_init();
	fuzz_pci_board();
	fuzz_pci_model();
	fuzz_claim(&port61);
	fuzz_claim_mmio(&flash);
	Status = 0x80;
	Mode = ARRAY;
	for (int i = 0; i < 4; i++)
		SET_BDA(port_com[i], Spec.com[i]);
	SET_BDA(port_lpt[0], Spec.lpt);
	SET_BDA(equipment_list_flags, Spec.mouse ? 0x04 : 0);
	LegacyRamSize = Spec.ram;
}

/****************************************************************
 * Cases
 ****************************************************************/

/* Flash ESCD before the POST: erased, valid (as the POST builds it, or with boards of an OS in virtual
   slots and an ISA slot), then corrupt: checksum, size, signature, board lengths, random. */
enum { E_ERASED, E_SAME, E_OS_BOARDS, E_BAD_SUM, E_BAD_SIZE, E_BAD_SIG, E_BAD_BOARDS, E_RANDOM, NCONTENTS };
static const char *const ContentNames[NCONTENTS] = {
	"erased", "valid (POST's)", "valid with OS boards", "bad checksum", "bad size", "bad signature",
	"bad board lengths", "random"
};

static void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static u16 get16(const u8 *p) { return p[0] | p[1] << 8; }

static u16 sum16(const u8 *p, int n)
{
	u16 sum = 0;
	while (n-- > 0)
		sum += *p++;
	return sum;
}

/* An ESCD of 'boards' board records of slot slots[i], each with one function of an I/O port. */
static int escd_make(u8 *e, int boards, const u8 *slots)
{
	int len = 12;
	memcpy(e + 2, "ACFG\x01\x02", 6);
	e[8] = boards;
	e[9] = e[10] = e[11] = 0;
	for (int b = 0; b < boards; b++) {
		u8 *r = e + len;
		static const u8 rec[] = { 0, 0, 0, 0, 0x41, 0xd0, 0x12, 0x34, 0x40, 0x40, 0, 0,
					  6, 0, 1, 0, 0x10, 0x07, 0x00, 0x03, 0, 0 };
		memcpy(r, rec, sizeof(rec));
		put16(r, sizeof(rec) + 2);
		r[2] = slots[b];
		put16(r + sizeof(rec), -sum16(r, sizeof(rec)));
		len += sizeof(rec) + 2;
	}
	put16(e, len + 2);
	put16(e + len, -sum16(e, len));
	return len + 2;
}

static const u8 OsSlots[3] = { 17, 3, 40 };     /* kept by the POST: 17 and 40 */

static void flash_content(void)
{
	u8 *e = &Flash[ESCD];
	int size;
	memset(Flash, 0xff, 0x10000);
	for (int i = 0; i < ESCD; i++)
		Flash[i] = fuzz_rand();                  /* CBFS data */
	switch (Spec.content) {
	case E_SAME:                                /* filled by the case: a first POST */
	case E_ERASED:
		break;
	case E_OS_BOARDS:
		escd_make(e, 3, OsSlots);
		break;
	case E_BAD_SUM:
		size = escd_make(e, 3, OsSlots);
		e[size - 1] ^= 1;
		break;
	case E_BAD_SIZE: {
		static const u16 sizes[] = { 0, 1, 13, 14, 4097, 0xffff, 0x8000 };
		escd_make(e, 3, OsSlots);
		put16(e, sizes[fuzz_rand() % 7]);
		break;
	}
	case E_BAD_SIG:
		escd_make(e, 3, OsSlots);
		e[3] = 'X';
		break;
	case E_BAD_BOARDS: {
		static const u16 lens[] = { 0, 1, 3, 4, 0xffff, 0x1000, 200 };
		size = escd_make(e, 3, OsSlots);
		put16(e + 12 + 24 * (fuzz_rand() % 3), lens[fuzz_rand() % 7]);
		e[8] = fuzz_rand() % 2 ? 255 : 3;
		put16(e + size - 2, 0);
		put16(e + size - 2, -sum16(e, size - 2));
		break;
	}
	default:
		for (int i = 0; i < 0x1000; i++)
			e[i] = fuzz_rand();
		if (fuzz_rand() % 2) {
			memcpy(e + 2, "ACFG", 4);
			put16(e, 14 + fuzz_rand() % 4083);
			size = get16(e);
			put16(e + size - 2, -sum16(e, size - 2));
		}
	}
}

static void random_config(void)
{
	static const u16 coms[] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8, 0 };
	static const u16 lpts[] = { 0x378, 0x278, 0x3bc, 0 };
	for (int i = 0; i < 4; i++)
		Spec.com[i] = coms[fuzz_rand() % 5];
	Spec.lpt = lpts[fuzz_rand() % 4];
	Spec.mouse = fuzz_rand() % 2;
	Spec.lpt_mode = fuzz_rand() % 3;
	Spec.board = (fuzz_rand() & 7) | Spec.lpt_mode << 3;
	Spec.ram = (1 + fuzz_rand() % 128) << 20;
	Spec.nldev = fuzz_rand() % 3 ? fuzz_rand() % 6 : fuzz_rand() % 24;
	u8 csn = 1;
	for (int i = 0; i < Spec.nldev; i++) {
		if (i && fuzz_rand() % 3 == 0)
			csn += fuzz_rand() % 3 ? 1 : 0;
		Spec.ldev[i].csn = csn;
		for (int k = 0; k < 8; k++) {
			Spec.ldev[i].id[k] = csn * 16 + k;
			Spec.ldev[i].io[k] = fuzz_rand() % 3 ? 0 : fuzz_rand() % 2 ? 0x200 + 8 * (fuzz_rand() % 64) : fuzz_rand();
			Spec.ldev[i].iolen[k] = fuzz_rand() % 4 ? 8 : fuzz_rand();
		}
		for (int k = 0; k < 2; k++) {
			Spec.ldev[i].irq[k] = fuzz_rand() % 2 ? 0 : fuzz_rand() % 16;
			Spec.ldev[i].dma[k] = fuzz_rand() % 2 ? 4 : fuzz_rand() % 8;
		}
	}
	Spec.csns = Spec.nldev ? csn : 0;
}

static void post(void)
{
	Writes = Erases = Programs = 0;
	for (int i = 0; i < Spec.nldev; i++)
		pnp_isa_ldev(Spec.ldev[i].csn, Spec.ldev[i].id, Spec.ldev[i].io, Spec.ldev[i].iolen, Spec.ldev[i].irq,
			     Spec.ldev[i].dma);
	fuzz_budget_us = fuzz_now_us + 60000000;
	pnp_finish(Spec.csns, 0x20b, Spec.board);
}

static int chip_known(void)
{
	return Spec.chip != OTHER && !(Spec.faults & F_BAD_ID);
}

static int chip_works(void)
{
	return chip_known() && !(Spec.faults & (Spec.chip == INTEL ? F_VPP | F_BUSY | F_ERASE_ERR | F_PROG_ERR : F_NEVER));
}

/****************************************************************
 * Checks
 ****************************************************************/

/* Resource data from p up to an end tag; its length, or fail. */
static int resources(const u8 *p, int max)
{
	int n = 0;
	while (n < max) {
		u8 tag = p[n];
		if (tag == 0x79)
			return n + 2;
		n += tag & 0x80 ? 3 + get16(p + n + 1) : 1 + (tag & 7);
	}
	fuzz_fail("node resources without an end tag");
}

static void check_nodes(void)
{
	int pos = 0, max = 0;
	int want = 9 + !!Spec.mouse + !!Spec.lpt + 1 + __builtin_popcount(Spec.board & 7);
	for (int i = 0; i < 4; i++)
		want += Spec.com[i] != 0;
	if (PnpNodeCount != want)
		fuzz_fail("%d device nodes, expected %d", PnpNodeCount, want);
	for (int n = 0; n < PnpNodeCount; n++) {
		const u8 *d = &PnpNodes[pos];
		int size = get16(d), len = 12;
		if (size < 18 || pos + size > (int)sizeof(PnpNodes) || d[2] != n || get16(d + 10) != 3)
			fuzz_fail("node %d at %d: size %d, handle %d, attributes %x", n, pos, size, d[2], get16(d + 10));
		for (int b = 0; b < 3; b++)
			len += resources(d + len, size - len);
		if (len != size)
			fuzz_fail("node %d: resources end at %d of %d", n, len, size);
		max = size > max ? size : max;
		pos += size;
	}
	if (max != PnpNodeMax)
		fuzz_fail("largest node %d, reported %d", max, PnpNodeMax);
}

static int escd_valid(const u8 *e)
{
	int size = get16(e);
	return size >= 14 && size <= 0x1000 && !memcmp(e + 2, "ACFG", 4) && !(u16)(sum16(e, size - 2) + get16(e + size - 2));
}

/* A valid ESCD in the flash: header, the system board (slot 0) and one board per ISA PnP card (slot =
   CSN, increasing) with their checksums and functions, nothing else. */
static void check_escd(const u8 *e)
{
	int size = get16(e), pos = 12, cards = 0, slot = 0, system = 0, csns = 0;
	if (!escd_valid(e) || e[6] != 1 || e[7] != 2)
		fuzz_fail("ESCD header or checksum (size %d)", size);
	for (int b = 0; b < e[8]; b++) {
		const u8 *r = e + pos;
		int len = get16(r), f = 12;
		if (pos + len > size - 2 || r[2] >= 16)
			fuzz_fail("ESCD board %d at %d: length %d, slot %d", b, pos, len, r[2]);
		if (len < 16 || (u16)(sum16(r, len - 2) + get16(r + len - 2)))
			fuzz_fail("ESCD board %d at %d: length %d or checksum", b, pos, len);
		if (r[2] ? r[2] <= slot : system++)
			fuzz_fail("ESCD board %d: slot %d after %d", b, r[2], slot);
		if (r[2])
			slot = r[2], cards++;
		while (get16(r + f)) {
			int flen = get16(r + f);
			if (r[f + 2] != 1 || f + 2 + flen > len - 4)
				fuzz_fail("ESCD board %d: function at %d, length %d", b, f, flen);
			f += 2 + flen;
		}
		if (f != len - 4)
			fuzz_fail("ESCD board %d: functions end at %d of %d", b, f, len);
		pos += len;
	}
	for (int i = 0; i < Spec.nldev; i++)
		csns += !i || Spec.ldev[i].csn != Spec.ldev[i - 1].csn;
	if (pos != size - 2 || !system || cards > csns || (csns && !cards))
		fuzz_fail("ESCD: boards end at %d of %d, system board %d, %d card boards of %d", pos, size, system,
			  cards, csns);
}

static void check_post(const u8 *before)
{
	check_nodes();
	if (!chip_known() && Writes > 12)
		fuzz_fail("%d writes to an unknown flash chip", Writes);
	if (Mode != ARRAY && !(Spec.faults & (F_BUSY | F_NEVER)))
		fuzz_fail("flash chip left in mode %d", Mode);
	if (chip_works()) {
		if (FlashChip != Spec.chip + 1)
			fuzz_fail("flash chip %d detected, expected %d", FlashChip, Spec.chip + 1);
		check_escd(&Flash[ESCD]);
	} else if (!chip_known()) {
		if (FlashChip || Erases || Programs)
			fuzz_fail("flash chip %d detected and %d erases, %d programs on a chip without the ID", FlashChip,
				  Erases, Programs);
		if (memcmp(before, Flash, 0x10000))
			fuzz_fail("flash changed although its chip is unknown");
	}
	if (memcmp(before, Flash, ESCD) || memcmp(before + ESCD + 0x1000, Flash + ESCD + 0x1000, 0x10000 - ESCD - 0x1000))
		fuzz_fail("flash outside the ESCD block changed");
}

/****************************************************************
 * Runtime calls
 ****************************************************************/

static u16 call(int prot, u16 a0, u16 a1, u16 a2, u16 a3, u16 a4, u16 a5, u16 a6)
{
	u16 args[8] = { a0, a1, a2, a3, a4, a5, a6, 0 };
	fuzz_budget_us = fuzz_now_us + 60000000;
	return handle_pnp(args, prot);
}

#define SEG(a) ((a) >> 4)
#define OFF(a) ((a) & 15)

static void expect(u16 got, u16 want, const char *what)
{
	if (got != want)
		fuzz_fail("%s returned %02x, expected %02x", what, got, want);
}

/* Bytes around the buffer at BUF (len bytes) must stay untouched. */
static void guard(int len, const char *what)
{
	u8 *b = fuzz_phys(BUF);
	for (int i = -64; i < 0; i++)
		if (b[i] != 0xcc)
			fuzz_fail("%s wrote before its buffer", what);
	for (int i = len; i < len + 64; i++)
		if (b[i] != 0xcc)
			fuzz_fail("%s wrote %d bytes past its buffer of %d", what, i - len + 1, len);
}

static void fill(void)
{
	memset(fuzz_phys(BUF - 64), 0xcc, 0x2000);
}

static void calls_valid(int prot)
{
	u8 *b = fuzz_phys(BUF);
	int prot_write = prot;
	/* 00h, then 01h on every node and past the last */
	fill();
	expect(call(prot, 0x00, OFF(BUF), SEG(BUF), OFF(BUF + 1), SEG(BUF + 1), 0, 0), 0, "00h");
	if (b[0] != PnpNodeCount || get16(b + 1) != PnpNodeMax)
		fuzz_fail("00h: %d nodes of at most %d bytes", b[0], get16(b + 1));
	guard(3, "00h");
	for (int n = 0; n < PnpNodeCount + 2; n++) {
		fill();
		b[0] = n;
		u16 r = call(prot, 0x01, OFF(BUF), SEG(BUF), OFF(BUF + 1), SEG(BUF + 1), 1 + fuzz_rand() % 2, 0);
		expect(r, n < PnpNodeCount ? 0 : 0x83, "01h");
		guard(r ? 1 : 1 + get16(b + 1), "01h");
		if (!r && b[0] != (n + 1 < PnpNodeCount ? n + 1 : 0xff))
			fuzz_fail("01h: next node %d after %d", b[0], n);
	}
	expect(call(prot, 0x02, 0, OFF(BUF), SEG(BUF), 1, 0, 0), 0x84, "02h on a static node");
	for (u16 f = 0x03; f <= 0x08; f++)
		expect(call(prot, f, OFF(BUF), SEG(BUF), 0, 0, 0, 0), 0x82, "03h-08h");
	expect(call(prot, 0x09, OFF(BUF), SEG(BUF), 0, 0, 0, 0), 0x8d, "09h");
	expect(call(prot, 0x0a, OFF(BUF), SEG(BUF), 0, 0, 0, 0), 0x8d, "0Ah");
	fill();
	expect(call(prot, 0x0b, OFF(BUF), SEG(BUF), OFF(BUF + 2), SEG(BUF + 2), 0, 0), 0, "0Bh");
	if (get16(b))
		fuzz_fail("0Bh: APM ID table of %d bytes", get16(b));
	guard(2, "0Bh");
	fill();
	expect(call(prot, 0x40, OFF(BUF), SEG(BUF), 0, 0, 0, 0), 0, "40h");
	if (b[0] != 1 || b[1] != Spec.csns || get16(b + 2) != (Spec.csns ? 0x20b : 0) || get16(b + 4))
		fuzz_fail("40h: %02x %02x %04x", b[0], b[1], get16(b + 2));
	guard(6, "40h");
	fill();
	expect(call(prot, 0x41, OFF(BUF), SEG(BUF), OFF(BUF + 2), SEG(BUF + 2), OFF(BUF + 4), SEG(BUF + 4)), 0, "41h");
	if (get16(b) != 0x1000 || get16(b + 2) != 0x1000 || (get16(b + 4) | get16(b + 6) << 16) != PNP_FLASH_BASE)
		fuzz_fail("41h: %x %x %x", get16(b), get16(b + 2), get16(b + 4) | get16(b + 6) << 16);
	guard(8, "41h");
	/* 42h: the flash ESCD; 43h: a valid one with an OS board written and read back */
	fill();
	u16 r = call(prot, 0x42, OFF(BUF), SEG(BUF), 0x1234, 0, 0, 0);
	if (!(Spec.faults & (F_BUSY | F_NEVER)))    /* else the chip reads its busy status */
		expect(r, escd_valid(&Flash[ESCD]) ? 0 : 0x8f, "42h");
	if (!r && memcmp(b, &Flash[ESCD], get16(b)))
		fuzz_fail("42h: not the flash ESCD");
	guard(r ? 0 : get16(b), "42h");
	if (prot_write) {
		expect(call(prot, 0x43, OFF(BUF), SEG(BUF), 0x1234, 0, 0, 0), 0x82, "43h in protected mode");
		return;
	}
	static const u8 slots[2] = { 0, 33 };
	int size = escd_make(b, 2, slots);
	u8 before[0x1000];
	memcpy(before, &Flash[ESCD], sizeof(before));
	r = call(prot, 0x43, OFF(BUF), SEG(BUF), 0, 0, 0, 0);
	expect(r, chip_works() ? 0 : chip_known() ? 0x8f : 0x82, "43h");
	if (!r && memcmp(&Flash[ESCD], b, size))
		fuzz_fail("43h: the flash ESCD is not the one written");
	if (r && !chip_known() && memcmp(&Flash[ESCD], before, sizeof(before)))
		fuzz_fail("43h: an unknown chip was written");
}

/* Garbage function numbers, node numbers, controls, buffer pointers and ESCDs. */
static void calls_garbage(void)
{
	static const u16 edges[] = { 0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0xfff0, 0xfffe, 0xffff };
	for (int c = 0; c < 64; c++) {
		u16 a[7];
		for (int i = 0; i < 7; i++)
			a[i] = fuzz_rand() % 2 ? edges[fuzz_rand() % 11] : fuzz_rand();
		a[0] = fuzz_rand() % 2 ? fuzz_rand() % 0x44 : a[0];
		for (int i = 2; i < 7; i += 2)              /* segments into mapped memory: below 640K */
			a[i] = fuzz_rand() % 2 ? a[i] % 0xa000 : 0x1000 + fuzz_rand() % 0x8000;
		if (a[0] == 0x43 && fuzz_rand() % 2) {      /* an ESCD there, often almost right */
			u8 *e = fuzz_phys(a[2] * 16 + a[1]);
			static const u8 slots[1] = { 20 };
			if (a[2] * 16 + a[1] + 0x1000 < 0xa0000 && a[2] * 16 + a[1] >= FUZZ_PHYS_LOW) {
				int size = escd_make(e, 1, slots);
				if (fuzz_rand() % 2)
					e[fuzz_rand() % size] ^= 1 << fuzz_rand() % 8;
			}
		}
		call(fuzz_rand() % 2, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
	}
}

/****************************************************************
 * Area
 ****************************************************************/

enum { C_CHIPS, C_CONTENT, C_REBOOT, C_CALLS, C_GARBAGE, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"POST: flash chip x fault", "POST: flash ESCD content", "POST twice: no second write",
	"runtime: every function", "runtime: garbage arguments",
};
static const unsigned ClassCount[NCLASSES] = { NCHIPS * 2 * (F_NFAULTS + 1), NCONTENTS * NCHIPS * 8, 300, 400, 2000 };

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
	random_config();
	Spec.chip = fuzz_rand() % 4 ? (fuzz_rand() % 2 ? W29 : INTEL) : OTHER;
	Spec.content = fuzz_rand() % NCONTENTS;
	Spec.faults = fuzz_rand() % 4 ? 0 : 1 << fuzz_rand() % F_NFAULTS;
	if (cls == C_CHIPS) {                       /* no fault or one, with the refresh bit or without */
		Spec.chip = i % NCHIPS;
		Spec.faults = (i / NCHIPS % (F_NFAULTS + 1) ? 1 << (i / NCHIPS % (F_NFAULTS + 1) - 1) : 0)
			      | (i / NCHIPS / (F_NFAULTS + 1) ? F_NO_REFRESH : 0);
		Spec.content = E_ERASED;
		if (Spec.faults & F_BUSY)
			Spec.faults = F_BUSY;           /* busy with the refresh bit stuck: 11 M reads */
	} else if (cls == C_CONTENT) {
		Spec.content = i % NCONTENTS;
		Spec.chip = i / NCONTENTS % NCHIPS;
		Spec.faults = 0;
	} else if (cls == C_REBOOT) {
		Spec.chip = i % 2 ? W29 : INTEL;
		Spec.faults = i / 2 % 2 ? F_NO_REFRESH : 0;
		Spec.content = fuzz_rand() % 2 ? E_OS_BOARDS : E_ERASED;
	}
	if (cls != C_CHIPS)
		Spec.faults &= ~F_BUSY;                 /* its 10 s erase timeout: tested by C_CHIPS only */
}

static void describe(unsigned index, char *buf, int size)
{
	static const char *const chips[NCHIPS] = { "Intel 28F001BX-T", "Winbond W29C010", "unknown chip" };
	static const char *const faults[F_NFAULTS] = { "Vpp low", "status busy", "erase error", "program error",
						       "page never written", "wrong ID", "refresh stuck" };
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "%s, ESCD %s, board %02x, COM %x %x %x %x, LPT %x mode %d, mouse %d, RAM %u MB, %d ISA ldevs",
			 chips[Spec.chip], ContentNames[Spec.content], Spec.board, Spec.com[0], Spec.com[1], Spec.com[2],
			 Spec.com[3], Spec.lpt, Spec.lpt_mode, Spec.mouse, Spec.ram >> 20, Spec.nldev);
	for (int f = 0; f < F_NFAULTS; f++)
		if (Spec.faults & 1 << f)
			n += snprintf(buf + n, size > n ? size - n : 0, ", %s", faults[f]);
}

static void run(unsigned index)
{
	u8 *before = _malloc(&ZoneHigh, 0x10000, 16);
	build(index);
	Flash = mmap(NULL, 0x10000, 3, 0x21, -1, 0);   /* PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS */
	if (Flash == (void *)-1)
		fuzz_fail("harness: mmap");
	flash_content();
	if (Spec.content == E_SAME || Spec.cls == C_REBOOT) {
		int pid = fork(), status;                /* first boot in a child: the second one starts afresh */
		if (pid < 0)
			fuzz_fail("harness: fork");
		if (!pid) {
			board_start();
			post();
			_exit(0);
		}
		if (waitpid(pid, &status, 0) != pid || status)
			fuzz_fail("first boot failed (status %x)", status);
	}
	memcpy(before, Flash, 0x10000);
	board_start();
	post();
	fuzz_post_us = fuzz_now_us;
	check_post(before);
	if (Spec.cls == C_REBOOT || (Spec.content == E_SAME && chip_works()))
		if (Erases || Programs || memcmp(before, Flash, 0x10000))
			fuzz_fail("second POST wrote the flash (%d erases, %d programs)", Erases, Programs);
	if (Spec.cls == C_CALLS)
		calls_valid(index % 2);
	else if (Spec.cls == C_GARBAGE)
		calls_garbage();
	if (memcmp(before, Flash, ESCD) || memcmp(before + ESCD + 0x1000, Flash + ESCD + 0x1000, 0x10000 - ESCD - 0x1000))
		fuzz_fail("flash outside the ESCD block changed");
}

const struct fuzz_area fuzz_pnp = { "pnp", NCLASSES, ClassNames, count, class_of, describe, run };
