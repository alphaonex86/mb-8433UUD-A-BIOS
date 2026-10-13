#include <types.h>
struct device {
	pci_devfn_t bdf;
};
struct device *pcidev_path_on_bus(unsigned int bus, pci_devfn_t devfn);
struct device *pcidev_on_root(u8 dev, u8 fn);
void pci_assign_irqs(struct device *dev, const unsigned char irqs[4]);
void pci_dev_write_config8(const struct device *dev, u16 reg, u8 value);
#ifndef __SIMPLE_DEVICE__
static inline void pci_write_config8(const struct device *dev, u16 reg, u8 value)
{
	pci_dev_write_config8(dev, reg, value);
}
#endif
