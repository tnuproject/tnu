#include <arch/pci.h>
#include <tnu/drivers.h>
#include <tnu/log.h>

void ahci_init(void)
{
    size_t found = 0;
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *dev = pci_get(i);
        if (dev->class_code == 0x01 && dev->subclass == 0x06) {
            found++;
            log_info("ahci", "controller at %02x:%02x.%u vendor=%04x device=%04x",
                     dev->bus, dev->slot, dev->function, dev->vendor_id, dev->device_id);
        }
    }
    if (!found) {
        log_info("ahci", "no AHCI controller detected");
    }
}
