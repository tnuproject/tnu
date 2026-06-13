#include <arch/io.h>
#include <arch/pci.h>
#include <tnu/log.h>

#define PCI_MAX_DEVICES 128

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t device_count;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xfc) | 0x80000000u;
    outl(0xcf8, address);
    return inl(0xcfc);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                       ((uint32_t)func << 8) | (offset & 0xfc) | 0x80000000u;
    outl(0xcf8, address);
    outl(0xcfc, value);
}

static void add_device(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_config_read32(bus, slot, func, 0x00);
    uint16_t vendor = id & 0xffff;
    if (vendor == 0xffff) {
        return;
    }
    if (device_count >= PCI_MAX_DEVICES) {
        return;
    }

    uint32_t class_reg = pci_config_read32(bus, slot, func, 0x08);
    uint32_t header_reg = pci_config_read32(bus, slot, func, 0x0c);
    uint32_t irq_reg = pci_config_read32(bus, slot, func, 0x3c);
    struct pci_device *dev = &devices[device_count++];
    dev->bus = bus;
    dev->slot = slot;
    dev->function = func;
    dev->vendor_id = vendor;
    dev->device_id = (id >> 16) & 0xffff;
    dev->prog_if = (class_reg >> 8) & 0xff;
    dev->subclass = (class_reg >> 16) & 0xff;
    dev->class_code = (class_reg >> 24) & 0xff;
    dev->header_type = (header_reg >> 16) & 0xff;
    dev->irq_line = irq_reg & 0xff;
    for (uint8_t i = 0; i < 6; i++) {
        dev->bars[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
    }
}

void pci_init(void)
{
    device_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read32((uint8_t)bus, slot, 0, 0x00);
            if ((id & 0xffff) == 0xffff) {
                continue;
            }
            uint8_t header = (pci_config_read32((uint8_t)bus, slot, 0, 0x0c) >> 16) & 0xff;
            uint8_t funcs = (header & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < funcs; func++) {
                add_device((uint8_t)bus, slot, func);
            }
        }
    }
    log_info("pci", "enumerated %llu device(s)", (uint64_t)device_count);
    for (size_t i = 0; i < device_count && i < 8; i++) {
        const struct pci_device *d = &devices[i];
        log_info("pci", "%02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x",
                 d->bus, d->slot, d->function, d->vendor_id, d->device_id,
                 d->class_code, d->subclass);
    }
}

size_t pci_count(void)
{
    return device_count;
}

const struct pci_device *pci_get(size_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

void pci_enable_bus_mastering(const struct pci_device *dev)
{
    if (!dev) {
        return;
    }
    uint32_t command = pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
    command |= 0x00000007u;
    pci_config_write32(dev->bus, dev->slot, dev->function, 0x04, command);
}

static uint8_t pci_find_capability(const struct pci_device *dev, uint8_t cap_id)
{
    if (!dev) {
        return 0;
    }
    uint32_t status_cmd = pci_config_read32(dev->bus, dev->slot, dev->function, 0x04);
    if (!(status_cmd & (1u << 20))) {
        return 0;
    }

    uint8_t ptr = (uint8_t)(pci_config_read32(dev->bus, dev->slot, dev->function, 0x34) & 0xfc);
    for (size_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint32_t cap = pci_config_read32(dev->bus, dev->slot, dev->function, ptr);
        if ((cap & 0xff) == cap_id) {
            return ptr;
        }
        ptr = (uint8_t)((cap >> 8) & 0xfc);
    }
    return 0;
}

int pci_set_power_state_d0(const struct pci_device *dev)
{
    uint8_t pm = pci_find_capability(dev, 0x01);
    if (!pm) {
        return -1;
    }
    uint32_t csr = pci_config_read32(dev->bus, dev->slot, dev->function, (uint8_t)(pm + 4));
    if ((csr & 0x3u) == 0) {
        return 0;
    }
    csr &= ~0x3u;
    pci_config_write32(dev->bus, dev->slot, dev->function, (uint8_t)(pm + 4), csr);
    for (volatile size_t i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    return 0;
}

int pci_disable_link_power_management(const struct pci_device *dev)
{
    uint8_t pcie = pci_find_capability(dev, 0x10);
    if (!pcie) {
        return -1;
    }
    uint8_t link_ctrl = (uint8_t)(pcie + 0x10);
    uint32_t value = pci_config_read32(dev->bus, dev->slot, dev->function, link_ctrl);
    value &= ~0x3u;
    pci_config_write32(dev->bus, dev->slot, dev->function, link_ctrl, value);
    return 0;
}
