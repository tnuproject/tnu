#include <arch/pci.h>
#include <tnu/log.h>
#include <tnu/usb.h>

static struct usb_controller_info controllers[USB_CONTROLLER_MAX];
static size_t controller_count;

static enum usb_controller_type type_from_prog_if(uint8_t prog_if)
{
    switch (prog_if) {
    case 0x00:
        return USB_CONTROLLER_UHCI;
    case 0x10:
        return USB_CONTROLLER_OHCI;
    case 0x20:
        return USB_CONTROLLER_EHCI;
    case 0x30:
        return USB_CONTROLLER_XHCI;
    default:
        return USB_CONTROLLER_UNKNOWN;
    }
}

const char *usb_controller_type_name(enum usb_controller_type type)
{
    switch (type) {
    case USB_CONTROLLER_UHCI:
        return "UHCI";
    case USB_CONTROLLER_OHCI:
        return "OHCI";
    case USB_CONTROLLER_EHCI:
        return "EHCI";
    case USB_CONTROLLER_XHCI:
        return "xHCI";
    default:
        return "unknown";
    }
}

static const char *driver_name(enum usb_controller_type type)
{
    switch (type) {
    case USB_CONTROLLER_UHCI:
        return "uhci-inventory";
    case USB_CONTROLLER_OHCI:
        return "ohci-inventory";
    case USB_CONTROLLER_EHCI:
        return "ehci-inventory";
    case USB_CONTROLLER_XHCI:
        return "xhci-inventory";
    default:
        return "unsupported";
    }
}

void usb_init(void)
{
    controller_count = 0;
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *dev = pci_get(i);
        if (dev->class_code != 0x0c || dev->subclass != 0x03) {
            continue;
        }
        if (controller_count >= USB_CONTROLLER_MAX) {
            log_warn("usb", "controller table full; skipping %02x:%02x.%u",
                     dev->bus, dev->slot, dev->function);
            continue;
        }
        struct usb_controller_info *info = &controllers[controller_count++];
        info->type = type_from_prog_if(dev->prog_if);
        info->bus = dev->bus;
        info->slot = dev->slot;
        info->function = dev->function;
        info->vendor_id = dev->vendor_id;
        info->device_id = dev->device_id;
        info->prog_if = dev->prog_if;
        info->driver = driver_name(info->type);
        info->hid_ready = false;
        log_info("usb", "%s controller %02x:%02x.%u vendor=%04x device=%04x driver=%s",
                 usb_controller_type_name(info->type), info->bus, info->slot,
                 info->function, info->vendor_id, info->device_id, info->driver);
    }
    if (!controller_count) {
        log_info("usb", "no USB controller detected");
    } else {
        log_info("usb", "USB HID keyboard support is not yet implemented");
        log_info("usb", "Use PS/2 keyboard emulation (firmware legacy mode) for now");
    }
}

size_t usb_controller_count(void)
{
    return controller_count;
}

const struct usb_controller_info *usb_controller_get(size_t index)
{
    return index < controller_count ? &controllers[index] : NULL;
}

bool usb_hid_keyboard_ready(void)
{
    for (size_t i = 0; i < controller_count; i++) {
        if (controllers[i].hid_ready) {
            return true;
        }
    }
    return false;
}
