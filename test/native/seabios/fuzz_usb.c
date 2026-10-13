/* USB above the host controller: a virtual controller (in the slot of the xHCI driver, which this build
   leaves out) carries a tree of device models - keyboards, mass storage, hubs - with faulty descriptors,
   ports and transfers, through usb.c enumeration, usb-hub.c, usb-hid.c, usb-msc.c, blockcmd.c and the
   block layer. The UHCI/OHCI/EHCI drivers themselves are in fuzz_usbhc.c. */
#include "biosvar.h"
#include "block.h"
#include "hw/pcidevice.h"
#include "hw/usb.h"
#include "hw/usb-hid.h"
#include "hw/usb-hub.h"
#include "malloc.h"
#include "output.h"
#include "std/disk.h"
#include "string.h"
#include "util.h"
#include "devfuzz.h"

int usb_process_op(struct disk_op_s *op);
extern struct usb_pipe *keyboard_pipe;

enum { NONE, KBD, MSC, HUB, OTHER };
enum {
	F_NEVER = 1 << 0,                           /* never answers (NAK forever / no handshake) */
	F_STALL_ALL = 1 << 1,
	F_STALL_CONFIG = 1 << 2,                    /* SET_CONFIGURATION and class requests stall */
	F_NO_ADDRESS = 1 << 3,                      /* acknowledges SET_ADDRESS, stays at address 0 */
	F_SHORT = 1 << 4,                           /* IN transfers stop half way (short packet) */
	F_LONG = 1 << 5,                            /* IN transfers overrun the buffer (babble) */
	F_FLAP = 1 << 6,                            /* its port connection status toggles */
	F_STUCK_RESET = 1 << 7,                     /* its port stays in reset */
	F_CSW_SIG = 1 << 8,
	F_CSW_TAG = 1 << 9,
	F_CSW_RESIDUE = 1 << 10,
	F_CSW_STATUS = 1 << 11,
	F_NOT_READY = 1 << 12,                      /* TEST UNIT READY always fails */
	F_SPEED = 1 << 13,                          /* hub port reports a garbage speed */
	F_NFAULTS = 14
};
#define F_UNTRUSTED (F_SHORT | F_CSW_SIG | F_CSW_TAG | F_CSW_RESIDUE | F_CSW_STATUS)

#define NDEV 12
#define NPORTS 16                               /* hub ports modelled; higher ones are empty */

struct udev {
	u8 kind, speed, addr, enumerated;
	unsigned faults;
	u8 dev[18], dev_len;                        /* device descriptor, bytes it returns */
	u8 cfg[512];
	u16 cfg_len;
	int child[NPORTS];                          /* hub: device index per port, -1 none */
	u16 port_status[NPORTS];
	unsigned long long reset_at[NPORTS];
	u8 hub_ports, hub_pwr;
	u8 pdt, maxlun;                             /* mass storage */
	u32 last_lba, blksize;
	u16 csw_residue_value;
	u8 csw_status, report_seed;
	/* mass storage transfer state */
	int state, data_len, data_pos, data_write;
	u32 tag, data_lba;
	u8 data[65536], status;                     /* real disks take 64 KB transfers (the setup benchmark) */
	int reports;
};

static struct {
	int cls, root_ports;
	int root[4];
	int ndev;
	struct udev d[NDEV];
} S;

static int DefaultDev = -1;                     /* the device just reset: answers at address 0 */
static int Keys;

/****************************************************************
 * Device models
 ****************************************************************/

static struct udev *by_address(int addr)
{
	if (!addr)
		return DefaultDev >= 0 ? &S.d[DefaultDev] : NULL;
	for (int i = 0; i < S.ndev; i++)
		if (S.d[i].addr == addr && S.d[i].kind)
			return &S.d[i];
	return NULL;
}

static int in_data(struct udev *u, u8 *buf, int want, const u8 *src, int have)
{
	if (u->faults & F_LONG && have >= want && want)
		return -1;                              /* babble */
	int n = have < want ? have : want;
	if (u->faults & F_SHORT)
		n /= 2;
	memcpy(buf, src, n);
	memset(buf + n, 0xa5, want - n);            /* the rest of the buffer is whatever was there */
	return 0;
}

static void port_reset_done(struct udev *hub, int p)
{
	int c = hub->child[p];
	hub->port_status[p] &= ~USB_PORT_STAT_RESET;
	if (c >= 0) {
		hub->port_status[p] |= USB_PORT_STAT_ENABLE;
		hub->port_status[p] &= ~USB_PORT_STAT_SPEED_MASK;
		hub->port_status[p] |= S.d[c].speed == USB_LOWSPEED ? USB_PORT_STAT_LOW_SPEED
			: S.d[c].speed == USB_HIGHSPEED ? USB_PORT_STAT_HIGH_SPEED : 0;
		if (S.d[c].faults & F_SPEED)
			hub->port_status[p] |= USB_PORT_STAT_SPEED_MASK;
		S.d[c].addr = 0;
		DefaultDev = c;
	}
}

static int hub_request(struct udev *u, const struct usb_ctrlrequest *req, u8 *data)
{
	int p = req->wIndex - 1;
	if (req->bRequestType == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE)) {
		u8 desc[9] = { 9, 0x29, u->hub_ports, 0, 0, u->hub_pwr, 0, 0, 0xff };
		return in_data(u, data, req->wLength, desc, 9);
	}
	if (p < 0 || p >= u->hub_ports)
		return -1;
	if (p >= NPORTS)
		return req->bRequest == USB_REQ_GET_STATUS ? in_data(u, data, req->wLength, (u8[4]){ 0 }, 4) : 0;
	struct udev *c = u->child[p] >= 0 ? &S.d[u->child[p]] : NULL;
	switch (req->bRequest) {
	case USB_REQ_GET_STATUS: {
		if (u->port_status[p] & USB_PORT_STAT_RESET && fuzz_now_us >= u->reset_at[p]
		    && !(c && c->faults & F_STUCK_RESET))
			port_reset_done(u, p);
		if (c && c->faults & F_FLAP)
			u->port_status[p] ^= USB_PORT_STAT_CONNECTION;
		u8 sts[4] = { u->port_status[p], u->port_status[p] >> 8, 0, 0 };
		return in_data(u, data, req->wLength, sts, 4);
	}
	case USB_REQ_SET_FEATURE:
		if (req->wValue == USB_PORT_FEAT_POWER && c)
			u->port_status[p] |= USB_PORT_STAT_POWER | USB_PORT_STAT_CONNECTION;
		if (req->wValue == USB_PORT_FEAT_RESET) {
			u->port_status[p] |= USB_PORT_STAT_RESET;
			u->reset_at[p] = fuzz_now_us + 10000;
		}
		return 0;
	case USB_REQ_CLEAR_FEATURE:
		if (req->wValue == USB_PORT_FEAT_ENABLE)
			u->port_status[p] &= ~USB_PORT_STAT_ENABLE;
		return 0;
	}
	return -1;
}

static int control(struct udev *u, const struct usb_ctrlrequest *req, u8 *data)
{
	int type = req->wValue >> 8;
	if (req->bRequestType == USB_DIR_IN && req->bRequest == USB_REQ_GET_DESCRIPTOR) {
		if (type == USB_DT_DEVICE)
			return in_data(u, data, req->wLength, u->dev, u->dev_len);
		if (type == USB_DT_CONFIG)
			return in_data(u, data, req->wLength, u->cfg, u->cfg_len);
		return -1;
	}
	if (req->bRequestType == USB_DIR_OUT && req->bRequest == USB_REQ_SET_ADDRESS) {
		if (!(u->faults & F_NO_ADDRESS))
			u->addr = req->wValue;
		DefaultDev = -1;
		return 0;
	}
	if (u->faults & F_STALL_CONFIG)
		return -1;
	if (req->bRequestType == USB_DIR_OUT && req->bRequest == USB_REQ_SET_CONFIGURATION) {
		u->enumerated = 1;
		return 0;
	}
	if (u->kind == HUB && (req->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS)
		return hub_request(u, req, data);
	if (u->kind == MSC && req->bRequest == 0xfe)
		return in_data(u, data, req->wLength, &u->maxlun, 1);
	if (u->kind == KBD && (req->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS)
		return 0;                               /* SET_PROTOCOL, SET_IDLE */
	return -1;
}

static void put_be32(u8 *p, u32 v)
{
	p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

/* Bulk-only transport: CBW, data, CSW. */
static void scsi_command(struct udev *u, const u8 *cbw)
{
	const u8 *cdb = cbw + 15;
	u32 lba = (u32)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5], blocks = cdb[7] << 8 | cdb[8];
	memcpy(&u->tag, cbw + 4, 4);
	u->data_len = u->data_pos = u->data_write = 0;
	u->status = 0;
	memset(u->data, 0, sizeof(u->data));
	switch (cdb[0]) {
	case 0x12:                                  /* INQUIRY */
		u->data[0] = u->pdt, u->data[1] = 0x80;
		memcpy(u->data + 8, "DEVFUZZ USB STICK       1.0 ", 28);
		u->data_len = 36;
		break;
	case 0x00:                                  /* TEST UNIT READY */
		u->status = u->faults & F_NOT_READY ? 1 : 0;
		break;
	case 0x03:                                  /* REQUEST SENSE */
		u->data[0] = 0x70, u->data[2] = u->faults & F_NOT_READY ? 2 : 0, u->data[7] = 10;
		u->data_len = 18;
		break;
	case 0x25:                                  /* READ CAPACITY(10) */
		put_be32(u->data, u->last_lba);
		put_be32(u->data + 4, u->blksize);
		u->data_len = 8;
		break;
	case 0x28:                                  /* READ(10) */
	case 0x2a:                                  /* WRITE(10) */
		if ((u64)lba + blocks > (u64)u->last_lba + 1 || blocks * 512 > sizeof(u->data)) {
			u->status = 1;
			break;
		}
		u->data_lba = lba;
		u->data_len = blocks * 512;
		u->data_write = cdb[0] == 0x2a;
		for (u32 b = 0; !u->data_write && b < blocks; b++)
			for (int k = 0; k < 8; k++)
				u->data[b * 512 + k] = (u64)(lba + b) >> (8 * k);
		break;
	default:
		u->status = 1;
	}
	u->state = 1;
}

static int bulk(struct udev *u, int dir, u8 *buf, int len)
{
	if (u->kind != MSC)
		return -1;
	if (dir == USB_DIR_OUT && u->state == 0) {
		u32 sig;
		memcpy(&sig, buf, 4);
		if (len != 31 || sig != 0x43425355)
			return -1;
		scsi_command(u, buf);
		return 0;
	}
	if (u->state == 1 && u->data_pos < u->data_len) {
		int n = u->data_len - u->data_pos < len ? u->data_len - u->data_pos : len;
		if (u->data_write) {
			for (int k = 0; k < n; k += 512) {
				u64 stamp = 0;
				for (int b = 0; b < 8; b++)
					stamp |= (u64)buf[k + b] << (8 * b);
				if (stamp != u->data_lba + (u->data_pos + k) / 512)
					fuzz_fail("USB write meant for LBA %llu landed on LBA %u", stamp,
						  u->data_lba + (u->data_pos + k) / 512);
			}
		} else if (in_data(u, buf, len, u->data + u->data_pos, u->data_len - u->data_pos)) {
			return -1;
		}
		u->data_pos += n;
		if (u->data_pos >= u->data_len)
			u->state = 2;
		return 0;
	}
	if (dir == USB_DIR_IN && u->state >= 1) {   /* CSW */
		u8 csw[13];
		u32 sig = u->faults & F_CSW_SIG ? 0x55534253 : 0x53425355;
		u32 tag = u->faults & F_CSW_TAG ? ~u->tag : u->tag;
		u32 residue = u->faults & F_CSW_RESIDUE ? 0xffffffff : u->data_len - u->data_pos;
		memcpy(csw, &sig, 4), memcpy(csw + 4, &tag, 4), memcpy(csw + 8, &residue, 4);
		csw[12] = u->faults & F_CSW_STATUS ? u->csw_status : u->status;
		u->state = 0;
		return in_data(u, buf, len, csw, 13);
	}
	return -1;
}

/****************************************************************
 * Virtual host controller (the xHCI driver entry points of usb.c)
 ****************************************************************/

static struct usb_s Cntl;
static struct usbhub_s RootHub;

static int root_detect(struct usbhub_s *hub, u32 port)
{
	int c = S.root[port];
	if (c < 0)
		return timer_check(hub->detectend) ? -1 : 0;
	if (S.d[c].faults & F_FLAP)
		return fuzz_rand() % 2;
	return 1;
}

static int root_reset(struct usbhub_s *hub, u32 port)
{
	int c = S.root[port];
	msleep(USB_TIME_DRST);
	if (c < 0 || (S.d[c].faults & F_STUCK_RESET))
		return -1;
	S.d[c].addr = 0;
	DefaultDev = c;
	return S.d[c].speed;
}

static void root_disconnect(struct usbhub_s *hub, u32 port) { }

static struct usbhub_op_s RootOps = { .detect = root_detect, .reset = root_reset, .disconnect = root_disconnect };

void xhci_setup(void)
{
	if (!S.root_ports)
		return;
	Cntl.type = USB_TYPE_XHCI;
	RootHub.cntl = &Cntl;
	RootHub.op = &RootOps;
	RootHub.portcount = S.root_ports;
	usb_enumerate(&RootHub);
}

struct usb_pipe *xhci_realloc_pipe(struct usbdevice_s *usbdev, struct usb_pipe *upipe,
				   struct usb_endpoint_descriptor *epdesc)
{
	if (!epdesc) {
		free(upipe);
		return NULL;
	}
	struct usb_pipe *pipe = upipe ?: malloc_high(sizeof(*pipe));
	if (!pipe)
		return NULL;
	usb_desc2pipe(pipe, usbdev, epdesc);
	return pipe;
}

int xhci_send_pipe(struct usb_pipe *pipe, int dir, const void *cmd, void *data, int datasize)
{
	struct udev *u = by_address(pipe->devaddr);
	if (!u || (u->faults & F_NEVER)) {
		msleep(usb_xfer_time(pipe, datasize));
		return -1;
	}
	fuzz_tick(1000);
	if (u->faults & F_STALL_ALL)
		return -1;
	if (cmd) {
		if (pipe->ep || datasize != ((const struct usb_ctrlrequest *)cmd)->wLength)
			fuzz_fail("control transfer: endpoint %d, %d bytes for wLength %d", pipe->ep, datasize,
				  ((const struct usb_ctrlrequest *)cmd)->wLength);
		return control(u, cmd, data);
	}
	return bulk(u, dir, data, datasize);
}

int xhci_poll_intr(struct usb_pipe *pipe, void *data)
{
	struct udev *u = by_address(pipe->devaddr);
	fuzz_tick(100);
	if (!u || u->kind != KBD || u->reports >= 24 || (u->faults & (F_NEVER | F_STALL_ALL)))
		return -1;
	static const u8 e[] = { 0, 1, 0x04, 0x39, 0x53, 0x64, 0x65, 0x7f, 0x80, 0xe0, 0xe7, 0xff };
	u8 *report = data;
	for (int k = 0; k < pipe->maxpacket && k < 8; k++)
		report[k] = u->reports % 3 ? e[fuzz_rand() % sizeof(e)] : fuzz_rand();
	u->reports++;
	return 0;
}

int _cfunc32flat_xhci_poll_intr(struct usb_pipe *pipe, void *data)
{
	return xhci_poll_intr(pipe, data);
}

void process_key(u8 key)
{
	Keys++;
}

int bootprio_find_usb(struct usbdevice_s *usbdev, int lun) { return -1; }

/****************************************************************
 * Cases
 ****************************************************************/

static int new_dev(int kind)
{
	struct udev *u = &S.d[S.ndev];
	if (S.ndev == NDEV)
		return -1;
	memset(u, 0, sizeof(*u));
	u->kind = kind;
	u->speed = kind == KBD ? USB_LOWSPEED : kind == HUB ? USB_HIGHSPEED : USB_FULLSPEED;
	u8 dev[18] = { 18, 1, 0x00, 0x02, kind == HUB ? 9 : 0, 0, 0, kind == HUB ? 64 : 8, 0x34, 0x12, 0x78, 0x56,
		       0, 1, 0, 0, 0, 1 };
	memcpy(u->dev, dev, 18);
	u->dev_len = 18;
	u8 *c = u->cfg;
	int n = 9;
	u8 iface[9] = { 9, 4, 0, 0, 1, kind == KBD ? 3 : kind == MSC ? 8 : kind == HUB ? 9 : 7,
			kind == KBD ? 1 : kind == MSC ? 6 : 0, kind == KBD ? 1 : kind == MSC ? 0x50 : 0, 0 };
	memcpy(c + n, iface, 9), n += 9;
	if (kind == KBD || kind == HUB) {
		u8 ep[7] = { 7, 5, 0x81, 3, 8, 0, 10 };
		memcpy(c + n, ep, 7), n += 7;
	}
	if (kind == MSC) {
		u8 in[7] = { 7, 5, 0x81, 2, 64, 0, 0 }, out[7] = { 7, 5, 0x02, 2, 64, 0, 0 };
		memcpy(c + n, in, 7), n += 7;
		memcpy(c + n, out, 7), n += 7;
		c[9 + 4] = 2;
	}
	u8 cfg[9] = { 9, 2, n, n >> 8, 1, 1, 0, 0x80, 50 };
	memcpy(c, cfg, 9);
	u->cfg_len = n;
	u->hub_ports = 4, u->hub_pwr = 50;
	for (int p = 0; p < NPORTS; p++)
		u->child[p] = -1;
	u->pdt = 0, u->maxlun = 0, u->last_lba = 8191, u->blksize = 512;
	return S.ndev++;
}

static void plug(int hub, int port, int child)
{
	if (child < 0)
		return;
	if (hub < 0)
		S.root[port] = child;
	else if (port < NPORTS)
		S.d[hub].child[port] = child;
}

static void set_cfg_total(struct udev *u, u16 total)
{
	u->cfg[2] = total, u->cfg[3] = total >> 8;
}

enum { C_DEVDESC, C_CONFIG, C_ENDPOINTS, C_HUB, C_FAULTS, C_HID, C_MSC, C_CSW, C_MIX, C_NOMINAL, NCLASSES };
static const char *const ClassNames[NCLASSES] = {
	"device descriptor edges", "config/interface descriptor edges", "endpoints: size, count, interval",
	"hubs: 0-255 ports, port faults, nesting", "never answers, NAK, STALL, babble", "keyboard boot reports",
	"MSC INQUIRY/READ CAPACITY edges", "MSC CSW and data phase faults", "random fault mix",
	"nominal: a USB disk and keyboard must work (boot)",
};
static const unsigned ClassCount[NCLASSES] = { 5 * 6 * 3 * 3 * 3, 7 * 3 * 4 * 4 * 3, 9 * 3 * 4 * 2 * 3, 7 * 4 * 4 * 2,
					       7 * 3 * 2 * 2, 300, 5 * 5 * 5 * 4, 9 * 3 * 2, 3000, 4 };

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
	memset(&S, 0, sizeof(S));
	S.cls = cls;
	S.root_ports = 2;
	for (int p = 0; p < 4; p++)
		S.root[p] = -1;
	static const int kinds[3] = { KBD, MSC, HUB };
	switch (cls) {
	case C_NOMINAL: {                       /* disk + keyboard on the root ports, or both behind a hub */
		int disk = new_dev(MSC), kbd = new_dev(KBD);
		S.d[disk].last_lba = i / 2 ? 0xfffff : 8191;
		if (i % 2) {
			int hub = new_dev(HUB);
			plug(-1, 0, hub), plug(hub, 0, disk), plug(hub, 1, kbd);
		} else {
			plug(-1, 0, disk), plug(-1, 1, kbd);
		}
		break;
	}
	case C_DEVDESC: {
		static const u8 blen[5] = { 0, 1, 8, 18, 255 }, mps[6] = { 0, 1, 7, 8, 64, 255 }, got[3] = { 2, 8, 18 };
		static const u16 bcd[3] = { 0x0110, 0x0300, 0xffff };
		int d = new_dev(kinds[i / 270 % 3]);
		struct udev *u = &S.d[d];
		u->dev[0] = blen[i % 5];
		u->dev[7] = mps[i / 5 % 6];
		u->dev[2] = bcd[i / 30 % 3], u->dev[3] = bcd[i / 30 % 3] >> 8;
		u->dev_len = got[i / 90 % 3];
		plug(-1, 0, d);
		break;
	}
	case C_CONFIG: {
		static const u8 nif[3] = { 0, 1, 255 }, iflen[4] = { 0, 1, 9, 255 }, eplen[4] = { 0, 1, 7, 255 };
		int d = new_dev(kinds[i / 336 % 3]);
		struct udev *u = &S.d[d];
		u16 total[7] = { 0, 1, 8, 9, u->cfg_len, u->cfg_len + 1, 0xffff };
		set_cfg_total(u, total[i % 7]);
		u->cfg[4] = nif[i / 7 % 3];
		u->cfg[9] = iflen[i / 21 % 4];
		u->cfg[18] = eplen[i / 84 % 4];
		plug(-1, 1, d);
		break;
	}
	case C_ENDPOINTS: {
		static const u16 mps[9] = { 0, 1, 7, 8, 16, 17, 64, 1024, 0xffff };
		static const u8 ival[3] = { 0, 1, 255 }, neps[4] = { 0, 1, 2, 30 };
		int d = new_dev(kinds[i / 216 % 3]);
		struct udev *u = &S.d[d];
		int n = 18, eps = neps[i / 27 % 4];
		for (int e = 0; e < eps; e++) {
			u8 ep[7] = { 7, 5, (e & 1 ? 0x80 : 0) | (e % 15 + 1), i / 108 % 2 ? 3 : 2, mps[i % 9],
				     mps[i % 9] >> 8, ival[i / 9 % 3] };
			memcpy(u->cfg + n, ep, 7), n += 7;
		}
		u->cfg[13] = eps;
		u->cfg_len = n;
		set_cfg_total(u, n);
		plug(-1, 0, d);
		break;
	}
	case C_HUB: {
		static const u8 ports[7] = { 0, 1, 4, 7, 15, 127, 255 };
		static const unsigned f[4] = { 0, F_FLAP, F_STUCK_RESET, F_SPEED };
		int h = new_dev(HUB);
		S.d[h].hub_ports = ports[i % 7];
		S.d[h].hub_pwr = i / 112 ? 255 : 0;
		int kind = (const int[]){ NONE, KBD, MSC, HUB }[i / 28 % 4];
		for (int p = 0; kind && p < 3; p++) {
			int c = new_dev(kind);
			S.d[c].faults = f[i / 7 % 4];
			plug(h, p, c);
		}
		plug(-1, 0, h);
		break;
	}
	case C_FAULTS: {
		static const unsigned f[7] = { F_NEVER, F_STALL_ALL, F_STALL_CONFIG, F_NO_ADDRESS, F_SHORT, F_LONG,
					       F_FLAP | F_STUCK_RESET };
		int d = new_dev(kinds[i / 7 % 3]);
		S.d[d].faults = f[i % 7];
		if (i / 21 % 2) {
			int h = new_dev(HUB);
			plug(-1, 0, h);
			plug(h, 1, d);
		} else {
			plug(-1, 0, d);
		}
		if (i / 42)
			plug(-1, 1, new_dev(KBD));
		break;
	}
	case C_HID:
		plug(-1, 0, new_dev(KBD));
		S.d[0].cfg[22] = (const u8[]){ 8, 16, 7, 17, 64 }[i % 5];
		break;
	case C_MSC: {
		static const u8 pdt[5] = { 0, 5, 0x1f, 0x7f, 0xff }, lun[4] = { 0, 1, 15, 255 };
		static const u32 last[5] = { 0, 1, 0x7fffffff, 0xfffffffe, 0xffffffff };
		static const u32 bs[5] = { 0, 1, 512, 4096, 0xffffffff };
		int d = new_dev(MSC);
		S.d[d].pdt = pdt[i % 5];
		S.d[d].last_lba = last[i / 5 % 5];
		S.d[d].blksize = bs[i / 25 % 5];
		S.d[d].maxlun = lun[i / 125 % 4];
		plug(-1, 0, d);
		break;
	}
	case C_CSW: {
		static const unsigned f[9] = { F_CSW_SIG, F_CSW_TAG, F_CSW_RESIDUE, F_CSW_STATUS, F_CSW_STATUS,
					       F_CSW_STATUS, F_NOT_READY, F_SHORT, F_LONG };
		int d = new_dev(MSC);
		S.d[d].faults = f[i % 9];
		S.d[d].csw_status = (const u8[]){ 1, 2, 0xff }[i / 9 % 3];
		if (i / 27)
			S.d[d].maxlun = 1;
		plug(-1, 0, d);
		break;
	}
	default:
		S.root_ports = 1 + fuzz_rand() % 4;
		for (int k = fuzz_rand() % 6; k > 0; k--) {
			int d = new_dev((const int[]){ KBD, MSC, HUB, OTHER }[fuzz_rand() % 4]);
			struct udev *u = &S.d[d];
			for (int f = fuzz_rand() % 3; f > 0; f--)
				u->faults |= 1 << fuzz_rand() % F_NFAULTS;
			for (int f = fuzz_rand() % 4; f > 0; f--) {
				static const u8 e[] = { 0, 1, 2, 7, 8, 9, 0x7f, 0x80, 0xff };
				int at = fuzz_rand() % 3 ? fuzz_rand() % u->cfg_len : fuzz_rand() % 18;
				(at < 18 && fuzz_rand() % 2 ? u->dev : u->cfg)[at] = fuzz_rand() % 2 ? e[fuzz_rand() % 9] : fuzz_rand();
			}
			u->hub_ports = fuzz_rand() % 2 ? fuzz_rand() % 8 : fuzz_rand();
			u->pdt = fuzz_rand() % 2 ? 0 : fuzz_rand();
			u->maxlun = fuzz_rand() % 2 ? 0 : fuzz_rand();
			u->last_lba = fuzz_rand() % 2 ? 8191 : fuzz_rand();
			u->blksize = fuzz_rand() % 2 ? 512 : fuzz_rand() % 2 ? 4096 : fuzz_rand();
			u->csw_status = fuzz_rand();
			int parent = -1;
			for (int h = 0; h < d; h++)
				if (S.d[h].kind == HUB && fuzz_rand() % 2)
					parent = h;
			plug(parent, fuzz_rand() % (parent < 0 ? S.root_ports : 4), d);
		}
	}
}

static const char *const FaultNames[F_NFAULTS] = {
	"never answers", "STALL all", "STALL config", "no address", "short IN", "babble", "port flaps",
	"port stuck in reset", "CSW bad signature", "CSW bad tag", "CSW residue FFFFFFFF", "CSW status",
	"not ready", "garbage port speed",
};

static void describe(unsigned index, char *buf, int size)
{
	static const char *const kind[] = { "none", "keyboard", "MSC", "hub", "other" };
	fuzz_seed(index);
	build(index);
	int n = snprintf(buf, size, "%d root ports:", S.root_ports);
	for (int p = 0; p < S.root_ports; p++)
		n += snprintf(buf + n, size > n ? size - n : 0, " %d", S.root[p]);
	for (int d = 0; d < S.ndev; d++) {
		struct udev *u = &S.d[d];
		n += snprintf(buf + n, size > n ? size - n : 0, "; dev %d %s dev[%u]", d, kind[u->kind], u->dev_len);
		for (int k = 0; k < 18 && n < size - 4; k++)
			n += snprintf(buf + n, size > n ? size - n : 0, " %02x", u->dev[k]);
		n += snprintf(buf + n, size > n ? size - n : 0, " cfg[%u]", u->cfg_len);
		for (int k = 0; k < u->cfg_len && k < 48 && n < size - 4; k++)
			n += snprintf(buf + n, size > n ? size - n : 0, " %02x", u->cfg[k]);
		if (u->kind == HUB)
			n += snprintf(buf + n, size > n ? size - n : 0, " ports %u pwr %u children", u->hub_ports, u->hub_pwr);
		for (int p = 0; u->kind == HUB && p < NPORTS && n < size - 4; p++)
			if (u->child[p] >= 0)
				n += snprintf(buf + n, size > n ? size - n : 0, " %d:%d", p, u->child[p]);
		if (u->kind == MSC)
			n += snprintf(buf + n, size > n ? size - n : 0, " pdt %02x last LBA %u blksize %u maxlun %u csw %02x", u->pdt,
				      u->last_lba, u->blksize, u->maxlun, u->csw_status);
		for (int f = 0; f < F_NFAULTS; f++)
			if (u->faults & 1 << f)
				n += snprintf(buf + n, size > n ? size - n : 0, ", %s", FaultNames[f]);
	}
}

/****************************************************************
 * Run and checks
 ****************************************************************/

struct usbdrive_s {                             /* usb-msc.c's drive */
	struct drive_s drive;
	struct usb_pipe *bulkin, *bulkout;
	int lun;
};

static struct udev *msc_of(struct drive_s *drive)
{
	struct usbdrive_s *udrive = container_of(drive, struct usbdrive_s, drive);
	struct udev *u = by_address(udrive->bulkin->devaddr);
	return u && u->kind == MSC ? u : NULL;
}

static void exercise_msc(struct drive_s *drive)
{
	struct udev *u = msc_of(drive);
	if (!u)
		fuzz_fail("USB disk registered without an enumerated mass storage device");
	if (drive->blksize != 512 || drive->sectors != (u64)u->last_lba + 1 || u->pdt & 0x1f)
		fuzz_fail("USB disk registered: %u-byte blocks, %llu sectors (device: pdt %02x, %u-byte blocks, last LBA %u)",
			  drive->blksize, drive->sectors, u->pdt, u->blksize, u->last_lba);
	map_hd_drive(drive);
	for (int k = 0; k < 3; k++) {
		u32 lba = (u32[]){ 0, u->last_lba, 7 }[k];
		int count = k == 2 ? 2 : 1;
		u8 *buf = _malloc(&ZoneLow, count * 512, 16);
		struct disk_op_s op = { .buf_fl = buf, .drive_fl = drive, .command = k == 2 ? CMD_WRITE : CMD_READ,
					.count = count, .lba = lba };
		for (int b = 0; op.command == CMD_WRITE && b < count; b++)
			for (int s = 0; s < 8; s++)
				buf[b * 512 + s] = (u64)(lba + b) >> (8 * s);
		fuzz_budget_us = fuzz_now_us + 60000000;
		int ret = usb_process_op(&op);
		if (op.count > count || (op.count < count && !ret))
			fuzz_fail("USB %s of %d sectors reports %u sectors done", k == 2 ? "write" : "read", count, op.count);
		if (!ret && (u->faults & (F_CSW_SIG | F_CSW_TAG)))
			fuzz_fail("USB transfer with a CSW of bad %s reported success",
				  u->faults & F_CSW_SIG ? "signature" : "tag");
		for (int b = 0; !ret && k < 2 && b < count && !(u->faults & F_UNTRUSTED); b++) {
			u64 got = 0;
			for (int s = 0; s < 8; s++)
				got |= (u64)buf[b * 512 + s] << (8 * s);
			if (got != (u64)lba + b)
				fuzz_fail("USB read of LBA %u returned LBA %llu without error", lba + b, got);
		}
		free(buf);
	}
}

static void run(unsigned index)
{
	build(index);
	fuzz_bios_init();
	fuzz_cmos_model();
	fuzz_pci_board();
	fuzz_pci_model();
	fuzz_setup_init();
	pci_probe_devices();
	fuzz_budget_us = fuzz_now_us + 600000000;
	usb_setup();
	wait_threads();
	fuzz_post_us = fuzz_now_us;
	int disks = 0;
	for (int b = 0; b < fuzz_nboots; b++)
		if (fuzz_boots[b].kind == 'H' || fuzz_boots[b].kind == 'C')
			if (++disks > 16 * S.ndev)
				fuzz_fail("%d USB drives registered for %d devices (a mass storage device has at most 16 LUNs)",
					  disks, S.ndev);
	disks = 0;
	for (int b = 0; b < fuzz_nboots; b++)
		if (fuzz_boots[b].kind == 'H') {
			disks++;
			exercise_msc(fuzz_boots[b].drive);
		}
	if (disks && (index % 16 == 0 || S.cls == C_NOMINAL)) {   /* the setup's disk benchmark on USB disks */
		fuzz_budget_us = fuzz_now_us + 600000000;
		fuzz_cpu_allow(3000000);
		if (fuzz_setup_disk_bench() != disks && S.cls == C_NOMINAL)
			fuzz_fail("disk benchmark failed on the healthy USB disk");
	}
	for (int i = 0; i < UsbListedCount; i++)
		if (UsbListed[i].parent >= i)
			fuzz_fail("USB list: entry %d has parent %d", i, UsbListed[i].parent);
	if (S.cls == C_NOMINAL) {
		int hubbed = S.ndev == 3, want = 1 + S.ndev, ok = 0;
		for (int i = 1; i < UsbListedCount; i++)
			ok += UsbListed[i].ok && UsbListed[i].parent == (hubbed && UsbListed[i].cls != 9 ? 1 : 0);
		if (UsbListedCount != want || UsbListed[0].parent != -1 || ok != S.ndev)
			fuzz_fail("nominal USB tree: %d entries (want %d), %d placed right", UsbListedCount, want, ok);
		char text[2048];
		fuzz_setup_usb_text(text, sizeof(text));
		const char *want_text = hubbed
			? " port 1: 1234:5678 hub, high speed\n  port 1: 1234:5678 storage, full speed\n"
			  "  port 2: 1234:5678 keyboard, low speed\n"
			: " port 1: 1234:5678 storage, full speed\n port 2: 1234:5678 keyboard, low speed\n";
		char *first = strchr(text, '\n');
		if (memcmp(text, "UHCI 00:", 8) && memcmp(text, "OHCI 00:", 8) && memcmp(text, "EHCI 00:", 8) && memcmp(text, "xHCI 00:", 8))
			fuzz_fail("USB page: controller line \"%.40s\"", text);
		if (!first || memcmp(first + 1, want_text, strlen(want_text)))
			fuzz_fail("USB page:\n%s", text);
	}
	if (S.cls == C_NOMINAL && (disks != 1 || !keyboard_pipe))
		fuzz_fail("nominal USB: %d disks registered (want 1), keyboard %s", disks, keyboard_pipe ? "found" : "missing");
	int kbd = 0;
	for (int d = 0; d < S.ndev; d++)
		kbd |= keyboard_pipe && S.d[d].kind == KBD && S.d[d].enumerated && S.d[d].addr == keyboard_pipe->devaddr;
	if (keyboard_pipe && !kbd)
		fuzz_fail("USB keyboard pipe on address %d, no such keyboard", keyboard_pipe->devaddr);
	if (keyboard_pipe && (keyboard_pipe->maxpacket < 8 || keyboard_pipe->maxpacket > 16))
		fuzz_fail("USB keyboard accepted with %d-byte reports", keyboard_pipe->maxpacket);
	fuzz_budget_us = fuzz_now_us + 60000000;
	for (int k = 0; k < 30; k++)
		usb_check_event();
	fuzz_setup_post_warnings();
}

const struct fuzz_area fuzz_usb = { "usb", NCLASSES, ClassNames, count, class_of, describe, run };
