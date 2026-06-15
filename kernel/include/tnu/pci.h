#ifndef TNU_PCI_H
#define TNU_PCI_H

#include <arch/pci.h>

struct pci_dev {
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
    int irq;
};

struct pci_device_id {
    uint16_t vendor;
    uint16_t device;
};

typedef int (*pci_probe_t)(struct pci_dev *dev);
typedef void (*pci_remove_t)(struct pci_dev *dev);

static inline int pci_enable_device(const struct pci_dev *dev)
{
    (void)dev;
    return 0;
}

static inline int pci_request_regions(const struct pci_dev *dev, const char *name)
{
    (void)dev;
    (void)name;
    return 0;
}

static inline void *pci_iomap(const struct pci_dev *dev, int bar, unsigned long len)
{
    (void)dev;
    (void)bar;
    (void)len;
    return NULL;
}

static inline int pci_register_driver(const struct pci_device_id *ids, pci_probe_t probe,
                                      pci_remove_t remove)
{
    (void)remove;
    for (size_t i = 0; i < pci_count(); ++i) {
        const struct pci_device *dev = pci_get(i);
        for (size_t j = 0; ids && (ids[j].vendor || ids[j].device); ++j) {
            if ((ids[j].vendor == 0xffffu || ids[j].vendor == dev->vendor_id) &&
                (ids[j].device == 0xffffu || ids[j].device == dev->device_id)) {
                struct pci_dev compat = {
                    .bus = dev->bus,
                    .slot = dev->slot,
                    .function = dev->function,
                    .vendor_id = dev->vendor_id,
                    .device_id = dev->device_id,
                    .class_code = dev->class_code,
                    .subclass = dev->subclass,
                    .prog_if = dev->prog_if,
                    .header_type = dev->header_type,
                    .irq_line = dev->irq_line,
                    .bars = { dev->bars[0], dev->bars[1], dev->bars[2],
                              dev->bars[3], dev->bars[4], dev->bars[5] },
                    .irq = dev->irq_line,
                };
                return probe(&compat);
            }
        }
    }
    return 0;
}

#endif
