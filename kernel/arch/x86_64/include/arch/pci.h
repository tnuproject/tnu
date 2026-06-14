#ifndef TNU_ARCH_PCI_H
#define TNU_ARCH_PCI_H

#include <tnu/types.h>

struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
    uint32_t bars[6];
};

void pci_init(void);
size_t pci_count(void);
const struct pci_device *pci_get(size_t index);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Simple inline helpers for PCI config access */
static inline uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (uint8_t)(pci_config_read32(bus, slot, func, offset & ~3) >> ((offset & 3) * 8));
}

static inline uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (uint16_t)(pci_config_read32(bus, slot, func, offset & ~1) >> ((offset & 1) * 8));
}

static inline void pci_write_config16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value)
{
    uint32_t old = pci_config_read32(bus, slot, func, offset & ~1);
    uint32_t shift = (offset & 1) * 8;
    uint32_t mask = 0xFFFF << shift;
    uint32_t new_val = (old & ~mask) | ((uint32_t)value << shift);
    pci_config_write32(bus, slot, func, offset & ~1, new_val);
}

void pci_enable_bus_mastering(const struct pci_device *dev);
int pci_set_power_state_d0(const struct pci_device *dev);
int pci_disable_link_power_management(const struct pci_device *dev);

#endif
