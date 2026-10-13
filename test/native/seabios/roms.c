/* Option ROM images built from a description, faulty fields included (shared by the PCI and ISA areas). */
#include "std/optionrom.h"
#include "std/pnpbios.h"
#include "string.h"
#include "devfuzz.h"

#define PCIR_AT 0x20
#define PNP_AT 0x40

void fuzz_rom_default(struct fuzz_rom *r, u16 vendor, u16 device, u8 blocks)
{
	memset(r, 0, sizeof(*r));
	r->size = blocks;
	r->pcioffset = PCIR_AT;
	r->vendor = vendor, r->device = device;
	r->ilen = blocks;
	r->indicator = 0x80;
	r->images = 1;
}

static void build_image(u8 *img, int room, const struct fuzz_rom *r)
{
	struct rom_header *h = (void *)img;
	int len = r->size * 512 < room ? r->size * 512 : room;
	if (room < 0x80)
		return;
	memset(img, 0, len > 0x80 ? len : 0x80);
	h->signature = r->sig_bad ? 0x55aa : OPTION_ROM_SIGNATURE;
	h->size = r->size;
	h->initVector[0] = 0xcb;                    /* retf: never run here */
	h->pcioffset = r->pcioffset;
	if (r->pnp) {
		struct pnp_data *pnp = (void *)(img + PNP_AT);
		h->pnpoffset = r->pnpoffset ?: PNP_AT;
		pnp->signature = r->pnp == 2 ? 0x12345678 : PNP_SIGNATURE;
		pnp->nextoffset = r->nextoffset;
		pnp->productname = 0x70;
		pnp->bev = r->bev;
		pnp->bcv = r->bcv;
		memcpy(img + 0x70, "DEVFUZZ", 8);
	}
	if (r->pcioffset + sizeof(struct pci_data) <= (unsigned)room && r->pcioffset >= 0x1c) {
		struct pci_data *pd = (void *)(img + r->pcioffset);
		pd->signature = r->pcir_bad ? 0x50434952 : PCI_ROM_SIGNATURE;
		pd->vendor = r->vendor;
		pd->device = r->device;
		pd->dlen = sizeof(*pd);
		pd->ilen = r->ilen;
		pd->type = r->type;
		pd->indicator = r->indicator;
	}
	if (len >= 512) {
		u8 sum = 0;
		for (int i = 0; i < len - 1; i++)
			sum += img[i];
		img[len - 1] = -sum + (r->checksum_bad ? 1 : 0);
	}
}

/* The image at img[0..room): one or two images (the second one at ilen blocks: x86, last). */
void fuzz_rom_build(u8 *img, int room, const struct fuzz_rom *r)
{
	build_image(img, room, r);
	if (r->images < 2 || !r->ilen || r->ilen * 512 >= room)
		return;
	struct fuzz_rom second = *r;
	second.type = 0, second.indicator = 0x80, second.sig_bad = second.pcir_bad = second.checksum_bad = 0;
	second.pcioffset = PCIR_AT;
	build_image(img + r->ilen * 512, room - r->ilen * 512, &second);
}
