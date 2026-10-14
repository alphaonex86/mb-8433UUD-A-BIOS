/* Floppy: an 82077-style controller (3F0-3F5, 3F7) with two drives, run through floppy.c's setup from
   the CMOS drive types, setup.c's POST check ("Floppy disk(s) fail") and disk accesses. */
#include "biosvar.h"
#include "block.h"
#include "hw/pcidevice.h"
#include "malloc.h"
#include "newbios_cmos.h"
#include "output.h"
#include "stacks.h"
#include "std/disk.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

void handle_0e(void);

enum {
	F_FLOAT = 1 << 0,                           /* no controller: the bus floats (FFh) */
	F_ZERO = 1 << 1,                            /* no controller: the bus reads 00h */
	F_RQM_NEVER = 1 << 2,                       /* controller never ready for a byte */
	F_IRQ_NEVER = 1 << 3,
	F_IRQ_SPURIOUS = 1 << 4,
	F_RESET_NO_IRQ = 1 << 5,
	F_RESULT_GARBAGE = 1 << 6,
	F_RESULT_SHORT = 1 << 7,
	F_RESULT_LONG = 1 << 8,
	F_DIO_STUCK = 1 << 9,                       /* wants to send while the BIOS sends a command */
	F_SLOW = 1 << 10,                           /* each command takes 3-7 s */
	F_NFAULTS = 11
};

struct spec {
	unsigned faults;
	u8 drive[2], media[2];                      /* drive present; media: 0 none, else FloppyInfo type */
	int setting_a, setting_b, swap, raw_cmos;   /* raw_cmos >= 0: CMOS 10h after setup's own write */
	u8 garbage[8];
};
static struct spec Spec;

/****************************************************************
 * Controller model
 ****************************************************************/

static struct {
	u8 dor, dsr_rate, msr_busy;
	u8 cmd[9], ncmd, want;
	u8 result[10], nresult, rpos;
	unsigned long long ready_at, irq_at;
	int pending_sense, reset_senses, irq_pending;
	u8 track[2];
} Fdc;

static const u8 Params[16] = { [3] = 2, [7] = 1, [8] = 0, [0xf] = 2, [0xa] = 1, [6] = 8, [5] = 8, [0xd] = 5 };
static const u8 MediaRate[9] = { 0, 1, 0, 2, 0, 3, 2, 1, 2 };  /* FloppyInfo data rates */

static void raise_irq(unsigned long long at)
{
	if (!(Spec.faults & F_IRQ_NEVER))
		Fdc.irq_at = at;
}

static void set_result(int n, const u8 *bytes)
{
	int sent = n + (Spec.faults & F_RESULT_LONG ? 2 : 0) - (Spec.faults & F_RESULT_SHORT && n > 1 ? 1 : 0);
	for (int i = 0; i < sent; i++)
		Fdc.result[i] = Spec.faults & F_RESULT_GARBAGE ? Spec.garbage[i % 8] : i < n ? bytes[i] : 0;
	Fdc.nresult = sent;
	Fdc.rpos = 0;
}

static void execute(void)
{
	u8 op = Fdc.cmd[0] & 0x1f, drv = Fdc.cmd[1] & 3, head = Fdc.cmd[1] >> 2 & 1;
	int there = drv < 2 && Spec.drive[drv], media = drv < 2 ? Spec.media[drv] : 0;
	unsigned long long done = fuzz_now_us + (Spec.faults & F_SLOW ? 3000000 + fuzz_rand() % 4000000 : 200);
	Fdc.ready_at = done;
	switch (op) {
	case 0x03:                                  /* SPECIFY */
		set_result(0, NULL);
		return;
	case 0x07:                                  /* RECALIBRATE */
	case 0x0f:                                  /* SEEK */
		if (there)
			Fdc.track[drv] = op == 0x07 ? 0 : Fdc.cmd[2];
		Fdc.pending_sense = there ? 0x20 | drv : 0x70 | drv;
		set_result(0, NULL);
		raise_irq(done);
		return;
	case 0x08: {                                /* SENSE INTERRUPT */
		if (Fdc.reset_senses)
			Fdc.pending_sense = 0xc0 | (4 - Fdc.reset_senses--);   /* after reset: one per drive */
		u8 r[2] = { Fdc.pending_sense, Fdc.track[Fdc.pending_sense & 1] };
		if (Fdc.pending_sense < 0) {
			set_result(1, (const u8[]){ 0x80 });
		} else {
			set_result(2, r);
			Fdc.pending_sense = -1;
		}
		Fdc.ready_at = fuzz_now_us;
		return;
	}
	case 0x0a:                                  /* READ ID */
	case 0x06:                                  /* READ */
	case 0x05:                                  /* WRITE */
	case 0x0d: {                                /* FORMAT */
		int ok = there && media && MediaRate[media] == Fdc.dsr_rate;
		u8 r[7] = { (ok ? 0 : 0x40) | head << 2 | drv, ok ? 0 : 0x01, 0, Fdc.track[drv & 1], head, 1, 2 };
		set_result(7, r);
		raise_irq(done);
		return;
	}
	default:
		set_result(1, (const u8[]){ 0x80 });
		Fdc.ready_at = fuzz_now_us;
	}
}

static unsigned fdc_read(u16 port, int size)
{
	if (Spec.faults & F_FLOAT)
		return 0xff;
	if (Spec.faults & F_ZERO)
		return 0;
	if (port == 0x3f4) {
		if ((Spec.faults & F_RQM_NEVER) || !(Fdc.dor & 4) || fuzz_now_us < Fdc.ready_at)
			return 0x10;
		int out = Fdc.rpos < Fdc.nresult || ((Spec.faults & F_DIO_STUCK) && Fdc.ncmd);
		return 0x80 | (out ? 0x50 : 0);
	}
	if (port == 0x3f5 && Fdc.rpos < Fdc.nresult && fuzz_now_us >= Fdc.ready_at)
		return Fdc.result[Fdc.rpos++];
	if (port == 0x3f7)
		return 0x7f;                            /* no disk change */
	return 0xff;
}

static void fdc_write(u16 port, unsigned value, int size)
{
	if (Spec.faults & (F_FLOAT | F_ZERO))
		return;
	if (port == 0x3f2) {
		if (!(Fdc.dor & 4) && (value & 4)) {
			Fdc.reset_senses = 4;
			Fdc.nresult = Fdc.rpos = Fdc.ncmd = 0;
			Fdc.ready_at = fuzz_now_us + 10;
			if (!(Spec.faults & F_RESET_NO_IRQ))
				raise_irq(fuzz_now_us + 10);
		}
		Fdc.dor = value;
	} else if (port == 0x3f4 || port == 0x3f7) {
		Fdc.dsr_rate = value & 3;
	} else if (port == 0x3f5 && (Fdc.dor & 4)) {
		if (!Fdc.ncmd)
			Fdc.want = 1 + Params[value & 0xf];
		Fdc.nresult = Fdc.rpos = 0;
		Fdc.cmd[Fdc.ncmd++] = value;
		if (Fdc.ncmd == Fdc.want || Fdc.ncmd == sizeof(Fdc.cmd)) {
			execute();
			Fdc.ncmd = 0;
		}
	}
}

/* yield(): the IRQ 6 line, through the PIC to handle_0e() when the DOR lets it out. */
static void fdc_irq_poll(void)
{
	if (Fdc.irq_at && fuzz_now_us >= Fdc.irq_at) {
		Fdc.irq_at = 0;
		Fdc.irq_pending = 1;
	}
	if ((Spec.faults & F_IRQ_SPURIOUS) && fuzz_rand() % 16 == 0)
		Fdc.irq_pending = 1;
	if (Fdc.irq_pending && (Fdc.dor & 8)) {
		Fdc.irq_pending = 0;
		handle_0e();
	}
}

static void models_start(void)
{
	static const struct fuzz_ports ports[] = { { 0x3f0, 0x3f5, fdc_read, fdc_write }, { 0x3f7, 0x3f7, fdc_read, fdc_write } };
	Fdc.pending_sense = -1;
	fuzz_claim(&ports[0]);
	fuzz_claim(&ports[1]);
	fuzz_irq_hook(fdc_irq_poll);
}

/****************************************************************
 * Cases
 ****************************************************************/

enum { C_TYPES, C_RAW_CMOS, C_FAULTS, C_GARBAGE, C_MIX, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"setup drive types A/B 0-15, swap", "CMOS 10h types 0-15 x 0-15", "no FDC, RQM, IRQ, DIO faults",
	"garbage result bytes", "random fault mix",
};
static const unsigned ClassCount[NCLASSES] = { 16 * 16 * 2, 256, (F_NFAULTS - 6) * 4 * 3 * 2 + 64, 8 * 8 * 4, 1500 };
static const u8 E8[] = { 0x00, 0x01, 0x7f, 0x80, 0xc0, 0xfe, 0xff, 0x20 };

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

static void healthy(int a, int b)
{
	Spec.setting_a = a, Spec.setting_b = b;
	Spec.drive[0] = a != 0, Spec.drive[1] = b != 0;
	Spec.media[0] = a > 0 && a < 6 ? a : 0;
	Spec.media[1] = b > 0 && b < 6 ? b : 0;
}

static void build(unsigned index)
{
	int cls = class_of(index);
	unsigned i = index;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(&Spec, 0, sizeof(Spec));
	Spec.raw_cmos = -1;
	healthy(4, 0);
	switch (cls) {
	case C_TYPES:
		healthy(i % 16, i / 16 % 16);
		Spec.swap = i / 256;
		break;
	case C_RAW_CMOS:
		Spec.raw_cmos = i;
		Spec.drive[0] = Spec.drive[1] = 1;
		Spec.media[0] = i >> 4 < 9 ? i >> 4 : 4, Spec.media[1] = (i & 15) < 9 ? i & 15 : 4;
		break;
	case C_FAULTS:
		if (i < 64) {                           /* drive or media missing, per drive */
			healthy(i % 6, i / 6 % 6);
			Spec.drive[0] &= !(i & 1) || i >= 36;
			Spec.media[1] = i / 36 ? 0 : Spec.media[1];
			break;
		}
		i -= 64;
		healthy((const u8[]){ 4, 0, 4, 2 }[i / 6 % 4], (const u8[]){ 0, 4, 3, 1 }[i / 6 % 4]);
		Spec.faults = (const unsigned[]){ F_FLOAT, F_ZERO, F_RQM_NEVER, F_IRQ_NEVER, F_RESET_NO_IRQ }[i % 5];
		if (i / 24 % 3 == 1)
			Spec.faults |= F_SLOW;
		Spec.drive[1] &= i / 72 == 0;
		if (i % 6 == 5)
			Spec.faults = F_DIO_STUCK;
		break;
	case C_GARBAGE:
		Spec.faults = F_RESULT_GARBAGE | (const unsigned[]){ 0, F_RESULT_SHORT, F_RESULT_LONG, F_IRQ_SPURIOUS }[i / 64];
		for (int k = 0; k < 8; k++)
			Spec.garbage[k] = k == 0 ? E8[i % 8] : k == 1 ? E8[i / 8 % 8] : E8[fuzz_rand() % 8];
		healthy(4, i % 2 ? 2 : 0);
		break;
	default:
		healthy(fuzz_rand() % 16, fuzz_rand() % 16);
		for (int k = fuzz_rand() % 3; k > 0; k--)
			Spec.faults |= 1 << fuzz_rand() % F_NFAULTS;
		for (int k = 0; k < 8; k++)
			Spec.garbage[k] = fuzz_rand() % 2 ? E8[fuzz_rand() % 8] : fuzz_rand();
		Spec.drive[0] ^= fuzz_rand() % 5 == 0, Spec.media[1] *= fuzz_rand() % 3 != 0;
		Spec.swap = fuzz_rand() % 2;
		if (fuzz_rand() % 4 == 0)
			Spec.raw_cmos = fuzz_rand() % 256;
	}
}

static const char *const FaultNames[F_NFAULTS] = {
	"bus floats", "bus reads 0", "RQM never", "IRQ never", "spurious IRQs", "no IRQ after reset",
	"garbage results", "short results", "long results", "DIO stuck", "slow commands",
};

static void describe(unsigned index, char *buf, int size)
{
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "setup A=%d B=%d swap %d raw CMOS 10h %d; drives %d/%d media %d/%d",
			 Spec.setting_a, Spec.setting_b, Spec.swap, Spec.raw_cmos, Spec.drive[0], Spec.drive[1],
			 Spec.media[0], Spec.media[1]);
	for (int f = 0; f < F_NFAULTS; f++)
		if (Spec.faults & 1 << f)
			n += snprintf(buf + n, size > n ? size - n : 0, ", %s", FaultNames[f]);
	if (Spec.faults & F_RESULT_GARBAGE)
		snprintf(buf + n, size > n ? size - n : 0, " %02x %02x %02x %02x", Spec.garbage[0], Spec.garbage[1], Spec.garbage[2],
			 Spec.garbage[3]);
}

/****************************************************************
 * Run and checks
 ****************************************************************/

static int access(struct drive_s *drive, int command, int lba, int count)
{
	u8 *buf = (void *)0x7c00;                   /* boot sector buffer: DMA-able */
	struct disk_op_s op = { .buf_fl = buf, .drive_fl = drive, .command = command, .count = count, .lba = lba };
	fuzz_budget_us = fuzz_now_us + 60000000;
	return floppy_process_op(&op);
}

static void run(unsigned index)
{
	build(index);
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	fuzz_pci_model();
	fuzz_setup_init();
	fuzz_set_setting(NB_FLOPPY_A, Spec.setting_a);
	fuzz_set_setting(NB_FLOPPY_B, Spec.setting_b);
	fuzz_set_setting(NB_SWAP_FLOPPY, Spec.swap);
	fuzz_set_setting(NB_HALT_ON, 1);           /* No Errors: no F1 wait (no keyboard here) in the timed POST check */
	fuzz_cmos_store();
	fuzz_setup_init();
	if (Spec.raw_cmos >= 0)
		fuzz_cmos[0x10] = Spec.raw_cmos;
	models_start();
	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 60000000;
	floppy_setup();

	int type[2] = { fuzz_cmos[0x10] >> 4, fuzz_cmos[0x10] & 15 }, n = 0;
	for (int b = 0; b < fuzz_nboots; b++) {
		struct drive_s *drive = fuzz_boots[b].drive;
		if (fuzz_boots[b].kind != 'F')
			continue;
		n++;
		if (drive->cntl_id > 1 || type[drive->cntl_id] != drive->floppy_type)
			fuzz_fail("floppy %u registered with type %u, CMOS says %d", drive->cntl_id, drive->floppy_type,
				  drive->cntl_id > 1 ? -1 : type[drive->cntl_id]);
	}
	int expected = (type[0] >= 1 && type[0] <= 8) + (type[1] >= 1 && type[1] <= 8);
	if (n != expected)
		fuzz_fail("%d floppies registered, CMOS 10h=%02x gives %d", n, fuzz_cmos[0x10], expected);

	unsigned long long warnings_us = fuzz_now_us;
	fuzz_setup_post_warnings();
	warnings_us = fuzz_now_us - warnings_us;
	int broken = Spec.faults & (F_FLOAT | F_ZERO | F_RQM_NEVER | F_IRQ_NEVER | F_RESET_NO_IRQ | F_DIO_STUCK);
	int bad_drive = (fuzz_setting(NB_FLOPPY_A) && (!Spec.drive[0] || broken))
		|| (fuzz_setting(NB_FLOPPY_B) && (!Spec.drive[1] || broken));
	int good = !Spec.faults && !bad_drive;
	if (fuzz_screen_has("Floppy disk(s) fail") != bad_drive && (!Spec.faults || broken)
	    && !(Spec.faults & F_IRQ_SPURIOUS))
		fuzz_fail("POST %s \"Floppy disk(s) fail\"", bad_drive ? "does not show" : "shows");
	if (good && warnings_us > 100000)          /* motors on since setup_init(): no spin-up wait at the prompt */
		fuzz_fail("floppy check at the boot prompt took %llu ms", warnings_us / 1000);
	fuzz_post_us = fuzz_now_us;

	for (int b = 0; b < fuzz_nboots; b++) {
		struct drive_s *drive = fuzz_boots[b].drive;
		if (fuzz_boots[b].kind != 'F')
			continue;
		int id = drive->cntl_id, ret = access(drive, CMD_READ, 0, 1);
		int readable = good && Spec.drive[id] && Spec.media[id] && type[id] <= 5 && Spec.raw_cmos < 0;
		if (readable && ret)
			fuzz_fail("reading a working floppy %d failed: %x", id, ret);
		if (!broken && !Spec.media[id] && !(Spec.faults & F_RESULT_GARBAGE) && !ret)
			fuzz_fail("reading floppy %d without media succeeded", id);
		access(drive, CMD_READ, 35, 2);
		access(drive, CMD_WRITE, 1, 1);
		access(drive, CMD_VERIFY, 0, 1);
		access(drive, CMD_FORMAT, 0, drive->lchs.sector);
		access(drive, CMD_RESET, 0, 0);
		for (int t = 0; t < 40; t++)
			floppy_tick();
	}
	fuzz_setup_screens();
}

const struct fuzz_area fuzz_fdc = { "fdc", NCLASSES, ClassNames, count, class_of, describe, run };
