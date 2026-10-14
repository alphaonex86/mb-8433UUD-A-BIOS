/* USB host controllers: register models of UHCI (I/O BAR 4), OHCI and EHCI (memory BAR 0) that are
   absent, float, never reset or halt, or whose root ports flap or stay in reset, through usb-uhci.c,
   usb-ohci.c and usb-ehci.c setup and root hub enumeration. The controllers never run their schedules:
   every transfer to a device found on a port times out, as with a dead device. A controller may be found
   running (a warm boot after an OS or boot loader used it: its schedule in RAM is garbage now): it must be
   stopped before the BIOS enables its bus mastering. */
#include "biosvar.h"
#include "hw/pcidevice.h"
#include "hw/usb.h"
#include "hw/usb-hid.h"
#include "output.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

#define UHCI_IO 0xe000
#define OHCI_MEM 0x1a000000
#define EHCI_MEM 0x1a100000

enum { UHCI, OHCI, EHCI, NHC };
enum {
	F_ABSENT_BAR = 1 << 0,                      /* BAR reads back all ones */
	F_ZERO_BAR = 1 << 1,
	F_FLOAT = 1 << 2,                           /* registers read all ones, writes are lost */
	F_ZERO = 1 << 3,                            /* registers read 0 */
	F_NO_RESET = 1 << 4,                        /* reset bit never clears */
	F_NO_HALT = 1 << 5,                         /* EHCI: HCHalted never set; UHCI: frame counter stopped */
	F_FLAP = 1 << 6,                            /* port connect status toggles */
	F_STUCK_RESET = 1 << 7,                     /* port reset never ends */
	F_SOC = 1 << 8,                             /* OHCI: scheduling overrun count bits set in HcCommandStatus */
	F_RUNNING = 1 << 9,                         /* found running its schedule (warm boot) */
	F_RESET_HALTS = 1 << 10,                    /* EHCI: halts on HCRESET only (the board's VIA after a warm boot) */
	F_NFAULTS = 11
};

struct hc {
	int present;
	unsigned faults;
	u8 ports, caplength;                        /* root ports reported (OHCI NDP, EHCI N_PORTS) */
	u8 device[16];                              /* per port: 0 none, 1 full, 2 low, 3 high speed */
	u32 reg[0x400];
	int roothub_read;
	unsigned long long reset_until[16];
	int hc_reset;                               /* HCRESET done */
};
static struct hc Hc[NHC];

static u32 hc_float(struct hc *h, u32 v)
{
	return h->faults & F_FLOAT ? 0xffffffff : h->faults & F_ZERO ? 0 : v;
}

/* UHCI: 32 bytes of 16-bit registers in I/O space. */
static unsigned uhci_read(u16 port, int size)
{
	struct hc *h = &Hc[UHCI];
	int r = port - UHCI_IO;
	u32 v = h->reg[r / 2];
	if (r == 0x06 && !(h->faults & F_NO_HALT))
		v = fuzz_now_us / 1000 & 0x7ff;         /* frame number: one per ms */
	if (r == 0x10 || r == 0x12) {
		int p = (r - 0x10) / 2, dev = h->device[p];
		v = (v & ~0x101) | (dev ? 1 : 0) | (dev == 2 ? 0x100 : 0);
		if (h->faults & F_FLAP)
			h->reg[r / 2] ^= 1, v ^= fuzz_rand() & 1;
	}
	if (r == 0x00 && !(h->faults & F_NO_RESET))
		v &= ~2;                                /* HCRESET done */
	return hc_float(h, size == 4 ? v | h->reg[r / 2 + 1] << 16 : v);
}

static void uhci_write(u16 port, unsigned value, int size)
{
	struct hc *h = &Hc[UHCI];
	int r = port - UHCI_IO;
	h->reg[r / 2] = value & 0xffff;
	if (size == 4)
		h->reg[r / 2 + 1] = value >> 16;
}

/* OHCI operational registers. */
static unsigned ohci_read(unsigned off, int size)
{
	struct hc *h = &Hc[OHCI];
	u32 v = h->reg[off / 4];
	if (off == 0x00)
		v = 0x10;
	else if (off == 0x34 && !v)
		v = 0x2edf;
	else if (off == 0x48)
		v = h->ports | 0x02000000, h->roothub_read = 1;
	else if (off >= 0x54 && off < 0x54 + 4 * 255) {
		int p = (off - 0x54) / 4, dev = p < 16 ? h->device[p] : 0;
		v = (v & ~(1 | 0x200)) | (dev ? 1 : 0) | (dev == 2 ? 0x200 : 0);
		if (p < 16 && (v & 0x10) && fuzz_now_us >= h->reset_until[p] && !(h->faults & F_STUCK_RESET))
			v = (v & ~0x10) | (dev ? 2 : 0), h->reg[off / 4] = v;
		if (h->faults & F_FLAP)
			v ^= fuzz_rand() & 1;
	} else if (off == 0x08) {
		v = (h->faults & F_NO_RESET ? v : v & ~1) | (h->faults & F_SOC ? 0x30000 : 0);
	}
	return hc_float(h, v) >> (8 * (off & 3));
}

static void ohci_write(unsigned off, unsigned value, int size)
{
	struct hc *h = &Hc[OHCI];
	if (off == 0x08 && (value & 1) && !(h->faults & F_NO_RESET))
		h->reg[0x04 / 4] = 0xc0;                /* HCR: HcControl reset, state USBSUSPEND */
	if (off >= 0x54 && off < 0x54 + 4 * 16 && (value & 0x10))
		h->reset_until[(off - 0x54) / 4] = fuzz_now_us + 10000;
	h->reg[off / 4] = value;
}

/* EHCI: capability registers, then the operational ones at CAPLENGTH. */
static unsigned ehci_read(unsigned off, int size)
{
	struct hc *h = &Hc[EHCI];
	u32 v;
	if (off < 0x0c) {
		u32 caps[3] = { h->caplength | 0x01000000, h->ports, 0 };
		v = caps[off / 4] >> (8 * (off & 3));
		return hc_float(h, v);
	}
	unsigned r = off - h->caplength;
	if (off < h->caplength)
		return hc_float(h, 0);
	v = h->reg[r / 4];
	if (r == 0x00 && !(h->faults & F_NO_RESET))
		v &= ~2;
	if (r == 0x04)
		v = (v & ~0x1000) | ((h->reg[0] & 1) && !(h->faults & F_NO_HALT) ? 0 : 0x1000)
			| ((h->reg[0] & 0x40) ? 0x20 : 0);  /* halted, async advance doorbell */
	if (r == 0x04 && (h->faults & F_RESET_HALTS) && !h->hc_reset)
		v &= ~0x1000;
	if (r >= 0x44 && r < 0x44 + 4 * 15) {
		int p = (r - 0x44) / 4, dev = h->device[p];
		v = (v & ~(1 | 4 | 0xc00)) | (dev ? 1 : 0) | (dev == 2 ? 0x400 : 0);
		if (dev == 3 && !(v & 0x100) && h->reset_until[p])
			v |= 4;                             /* high speed: enabled after reset */
		if (h->faults & F_FLAP)
			v ^= fuzz_rand() & 1;
	}
	return hc_float(h, v);
}

static void ehci_write(unsigned off, unsigned value, int size)
{
	struct hc *h = &Hc[EHCI];
	unsigned r = off - h->caplength;
	if (off < h->caplength || r >= sizeof(h->reg))
		return;
	if (r == 0x04) {
		h->reg[1] &= ~value;                    /* write 1 to clear */
		return;
	}
	if (r == 0x00 && (value & 0x40))
		value &= ~0x40;                         /* doorbell answered at once */
	if (r == 0x00 && (value & 2) && !(h->faults & F_NO_RESET)) {
		h->hc_reset = 1;                        /* HCRESET: the controller stops, halted */
		value &= ~1;
	} else if (r == 0x00 && (h->faults & F_RESET_HALTS) && !h->hc_reset)
		value |= 1;                             /* RUN clear ignored: still runs */
	if (r >= 0x44 && r < 0x44 + 4 * 15) {
		int p = (r - 0x44) / 4;
		if ((value & 0x100) && !(h->faults & F_STUCK_RESET))
			h->reset_until[p] = 1;
		if (h->faults & F_STUCK_RESET)
			value |= 0x100;
	}
	h->reg[r / 4] = value;
}

/* The controller would fetch its schedule now (UHCI RS, OHCI HCFS operational, EHCI RUN). */
static int hc_running(int k)
{
	struct hc *h = &Hc[k];
	return k == OHCI ? (h->reg[0x04 / 4] & 0xc0) == 0x80 : h->reg[0] & 1;
}

static void bus_master_check(unsigned bdf, unsigned command)
{
	int k = (bdf >> 3) - 0x0a;
	if (k >= 0 && k < NHC && Hc[k].present && (command & 4) && hc_running(k))
		fuzz_fail("bus mastering enabled on controller %d while it still runs the old schedule", k);
}

static void models_start(void)
{
	fuzz_pci_command_hook = bus_master_check;
	static const struct fuzz_ports uhci_ports = { UHCI_IO, UHCI_IO + 0x1f, uhci_read, uhci_write };
	static const struct fuzz_mmio ohci_mmio = { OHCI_MEM, 0x1000, ohci_read, ohci_write };
	static const struct fuzz_mmio ehci_mmio = { EHCI_MEM, 0x1000, ehci_read, ehci_write };
	static const u32 classes[NHC] = { 0x0c030000, 0x0c031000, 0x0c032000 };
	for (int k = 0; k < NHC; k++) {
		struct hc *h = &Hc[k];
		if (!h->present)
			continue;
		struct fuzz_pcidev *p = fuzz_pci_add((0x0a + k) << 3, 0x30381106 + k, classes[k], 0);
		int bar = k == UHCI ? 4 : 0;
		u32 base = k == UHCI ? UHCI_IO | 1 : k == OHCI ? OHCI_MEM : EHCI_MEM;
		if (h->faults & F_ABSENT_BAR)
			base = 0xffffffff;
		if (h->faults & F_ZERO_BAR)
			base &= 1;
		memcpy(&p->cfg[0x10 + 4 * bar], &base, 4);
	}
	fuzz_claim(&uhci_ports);
	fuzz_claim_mmio(&ohci_mmio);
	fuzz_claim_mmio(&ehci_mmio);
}

/****************************************************************
 * Cases
 ****************************************************************/

static const unsigned Faults[] = { 0, F_ABSENT_BAR, F_ZERO_BAR, F_FLOAT, F_ZERO, F_NO_RESET, F_NO_HALT, F_FLAP,
				   F_STUCK_RESET, F_FLAP | F_STUCK_RESET, F_SOC, F_RUNNING, F_RUNNING | F_RESET_HALTS };
#define NF (int)ARRAY_SIZE(Faults)
static const u8 PortCounts[] = { 0, 1, 2, 15, 255 };

enum { C_SINGLE, C_ALL, C_MIX, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"one controller: BAR, reset, halt, ports", "UHCI+OHCI+EHCI together", "random fault mix",
};
static const unsigned ClassCount[NCLASSES] = { NHC * NF * 5 * 4, NF * 4, 600 };

static int class_of(unsigned index)
{
	int c = 0;
	while (index >= ClassCount[c])
		index -= ClassCount[c++];
	return c;
}

static unsigned count(void)
{
	return ClassCount[0] + ClassCount[1] + ClassCount[2];
}

static void setup_hc(int k, unsigned faults, u8 ports, int device)
{
	struct hc *h = &Hc[k];
	h->present = 1;
	h->faults = faults;
	h->ports = k == UHCI ? 2 : k == EHCI ? ports & 15 : ports;
	h->caplength = 0x10;
	for (int p = 0; p < 16; p++)
		h->device[p] = p < 2 ? device : 0;
	if (faults & F_RUNNING) {                   /* RS, HCFS operational, RUN */
		h->reg[k == OHCI ? 0x04 / 4 : 0] = k == OHCI ? 0x80 : 1;
	}
}

static void build(unsigned index)
{
	int cls = class_of(index);
	unsigned i = index;
	for (int c = 0; c < cls; c++)
		i -= ClassCount[c];
	memset(Hc, 0, sizeof(Hc));
	switch (cls) {
	case C_SINGLE:
		setup_hc(i % NHC, Faults[i / NHC % NF], PortCounts[i / NHC / NF % 5], i / NHC / NF / 5);
		if (i % NHC == EHCI && i / NHC / NF / 5 == 3)
			Hc[EHCI].caplength = 0xff;
		break;
	case C_ALL:
		for (int k = 0; k < NHC; k++)
			setup_hc(k, Faults[i % NF], 2, k == EHCI ? 3 : i / NF);
		break;
	default:
		for (int k = 0; k < NHC; k++)
			if (fuzz_rand() % 2) {
				setup_hc(k, Faults[fuzz_rand() % NF] | Faults[fuzz_rand() % NF], fuzz_rand() % 2 ? 2 : fuzz_rand(),
					 fuzz_rand() % 4);
				for (int p = 0; p < 16; p++)
					Hc[k].device[p] = fuzz_rand() % 4;
				Hc[k].caplength = fuzz_rand() % 4 ? 0x10 : fuzz_rand();
			}
	}
}

static const char *const FaultNames[F_NFAULTS] = {
	"BAR all ones", "BAR zero", "registers float", "registers read 0", "never resets", "never halts/frame stopped",
	"ports flap", "port stuck in reset", "HcCommandStatus SOC bits set", "found running (warm boot)",
	"halts on HCRESET only",
};

static void describe(unsigned index, char *buf, int size)
{
	static const char *const names[NHC] = { "UHCI", "OHCI", "EHCI" };
	fuzz_seed(index);
	build(index);
	int n = 0;
	for (int k = 0; k < NHC; k++) {
		struct hc *h = &Hc[k];
		if (!h->present)
			continue;
		n += snprintf(buf + n, size > n ? size - n : 0, "%s%s ports %u caplength %02x devices", n ? "; " : "", names[k], h->ports,
			      h->caplength);
		for (int p = 0; p < 16; p++)
			n += snprintf(buf + n, size > n ? size - n : 0, "%u", h->device[p]);
		for (int f = 0; f < F_NFAULTS; f++)
			if (h->faults & 1 << f)
				n += snprintf(buf + n, size > n ? size - n : 0, ", %s", FaultNames[f]);
	}
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
	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 600000000;
	usb_setup();
	wait_threads();
	fuzz_post_us = fuzz_now_us;
	for (int b = 0; b < fuzz_nboots; b++)
		fuzz_fail("boot device registered behind a controller that never runs its schedule");
	if (Hc[OHCI].present && Hc[OHCI].faults == F_SOC && !Hc[OHCI].roothub_read)
		fuzz_fail("OHCI whose HcCommandStatus has other bits than HCR set never started");
	struct hc *e = &Hc[EHCI];   /* left running by a warm boot, halting on HCRESET only: still used (the board's case) */
	if (e->present && e->faults == (F_RUNNING | F_RESET_HALTS) && e->caplength == 0x10 && e->ports && e->device[0] == 3
	    && !e->reset_until[0])
		fuzz_fail("EHCI found running and halting only on HCRESET was left off: its ports were never checked");
	for (int k = 0; k < 10; k++)
		usb_check_event();
}

const struct fuzz_area fuzz_usbhc = { "usbhc", NCLASSES, ClassNames, count, class_of, describe, run };
