/* Seam for compiling coreboot_486 UM8881 code on the host: every coreboot header it includes maps here. */
#ifndef NATIVE_SEAM_TYPES_H
#define NATIVE_SEAM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u32 pci_devfn_t;

#define KiB (1 << 10)
#define MiB (1 << 20)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#define __weak __attribute__((weak))
#define __packed __attribute__((packed))
#define CONFIG(option) CONFIG_##option	/* the build passes the options used, from the board config */
enum cb_err { CB_SUCCESS = 0, CB_ERR = -1 };

#define PCI_DEV(bus, dev, fn) (((bus) << 20) | ((dev) << 15) | ((fn) << 12))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_CLASS_DEVICE 0x0a
#define PCI_BASE_CLASS_BRIDGE 0x06
#define PCI_CLASS_STORAGE_IDE 0x0101

#define BIOS_ERR 3
#define BIOS_WARNING 4
#define BIOS_INFO 6
#define BIOS_DEBUG 7
int printk(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define post_code(value) ((void)0)

#ifdef __SIMPLE_DEVICE__	/* else device/pci.h: struct device accessors */
u8 pci_read_config8(pci_devfn_t dev, u16 reg);
u16 pci_read_config16(pci_devfn_t dev, u16 reg);
void pci_write_config8(pci_devfn_t dev, u16 reg, u8 value);
void pci_write_config16(pci_devfn_t dev, u16 reg, u16 value);
static inline void pci_update_config8(pci_devfn_t dev, u16 reg, u8 mask, u8 or)
{
	pci_write_config8(dev, reg, (pci_read_config8(dev, reg) & mask) | or);
}
#endif

/* Sequential reads are only queued here (the queued call is pure: dropped when its value is unused);
   the model applies a queue in bulk, with the per-read semantics, before anything else it does. */
extern __thread uintptr_t seam_next_read;
u32 seam_read32(uintptr_t addr);
u32 seam_read32_queued(uintptr_t addr) __attribute__((pure));
static inline u32 read32p(uintptr_t addr)
{
	if (addr != seam_next_read)
		return seam_read32(addr);
	seam_next_read = addr + 4;
	return seam_read32_queued(addr);
}
void write32p(uintptr_t addr, u32 value);

uintptr_t cbmem_top_chipset(void);
void cbmem_initialize_empty(void);

#endif
