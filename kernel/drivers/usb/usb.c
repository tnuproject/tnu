#include <arch/pci.h>
#include <arch/keyboard.h>
#include <tnu/log.h>
#include <tnu/string.h>
#include <tnu/usb.h>

static struct usb_controller_info controllers[USB_CONTROLLER_MAX];
static size_t controller_count;
static bool hid_keyboard_seen;

struct hid_key_map {
    uint8_t usage;
    uint8_t set1;
    bool extended;
};

static const struct hid_key_map hid_key_map[] = {
    { 0x04, 0x1e, false }, { 0x05, 0x30, false },
    { 0x06, 0x2e, false }, { 0x07, 0x20, false },
    { 0x08, 0x12, false }, { 0x09, 0x21, false },
    { 0x0a, 0x22, false }, { 0x0b, 0x23, false },
    { 0x0c, 0x17, false }, { 0x0d, 0x24, false },
    { 0x0e, 0x25, false }, { 0x0f, 0x26, false },
    { 0x10, 0x32, false }, { 0x11, 0x31, false },
    { 0x12, 0x18, false }, { 0x13, 0x19, false },
    { 0x14, 0x10, false }, { 0x15, 0x13, false },
    { 0x16, 0x1f, false }, { 0x17, 0x14, false },
    { 0x18, 0x16, false }, { 0x19, 0x2f, false },
    { 0x1a, 0x11, false }, { 0x1b, 0x2d, false },
    { 0x1c, 0x15, false }, { 0x1d, 0x2c, false },
    { 0x1e, 0x02, false }, { 0x1f, 0x03, false },
    { 0x20, 0x04, false }, { 0x21, 0x05, false },
    { 0x22, 0x06, false }, { 0x23, 0x07, false },
    { 0x24, 0x08, false }, { 0x25, 0x09, false },
    { 0x26, 0x0a, false }, { 0x27, 0x0b, false },
    { 0x28, 0x1c, false }, { 0x29, 0x01, false },
    { 0x2a, 0x0e, false }, { 0x2b, 0x0f, false },
    { 0x2c, 0x39, false }, { 0x2d, 0x0c, false },
    { 0x2e, 0x0d, false }, { 0x2f, 0x1a, false },
    { 0x30, 0x1b, false }, { 0x31, 0x2b, false },
    { 0x32, 0x2b, false }, { 0x33, 0x27, false },
    { 0x34, 0x28, false }, { 0x35, 0x29, false },
    { 0x36, 0x33, false }, { 0x37, 0x34, false },
    { 0x38, 0x35, false }, { 0x39, 0x3a, false },
    { 0x3a, 0x3b, false }, { 0x3b, 0x3c, false },
    { 0x3c, 0x3d, false }, { 0x3d, 0x3e, false },
    { 0x3e, 0x3f, false }, { 0x3f, 0x40, false },
    { 0x40, 0x41, false }, { 0x41, 0x42, false },
    { 0x42, 0x43, false }, { 0x43, 0x44, false },
    { 0x44, 0x57, false }, { 0x45, 0x58, false },
    { 0x4a, 0x47, true  }, { 0x4b, 0x4b, true  },
    { 0x4c, 0x53, true  }, { 0x4d, 0x4f, true  },
    { 0x4f, 0x4d, true  }, { 0x50, 0x4b, true  },
    { 0x51, 0x50, true  }, { 0x52, 0x48, true  },
    { 0x53, 0x45, false },
};

static uint8_t hid_last_modifiers;
static uint8_t hid_last_keys[6];

static bool hid_has_key(const uint8_t keys[6], uint8_t usage)
{
    for (size_t i = 0; i < 6; i++) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

static bool hid_usage_to_set1(uint8_t usage, uint8_t *set1, bool *extended)
{
    for (size_t i = 0; i < sizeof(hid_key_map) / sizeof(hid_key_map[0]); i++) {
        if (hid_key_map[i].usage == usage) {
            *set1 = hid_key_map[i].set1;
            *extended = hid_key_map[i].extended;
            return true;
        }
    }
    return false;
}

static bool hid_modifier_to_set1(uint8_t bit, uint8_t *set1, bool *extended)
{
    switch (bit) {
    case 0:
        *set1 = 0x1d; *extended = false; return true; /* left ctrl */
    case 1:
        *set1 = 0x2a; *extended = false; return true; /* left shift */
    case 2:
        *set1 = 0x38; *extended = false; return true; /* left alt */
    case 4:
        *set1 = 0x1d; *extended = true; return true;  /* right ctrl */
    case 5:
        *set1 = 0x36; *extended = false; return true; /* right shift */
    case 6:
        *set1 = 0x38; *extended = true; return true;  /* right alt */
    default:
        return false;
    }
}

static void hid_emit_set1(uint8_t set1, bool extended, bool release)
{
    uint8_t scancode = release ? (uint8_t)(set1 | 0x80) : set1;
    if (extended) {
        keyboard_inject_extended_set1_scancode(scancode);
    } else {
        keyboard_inject_set1_scancode(scancode);
    }
}

void usb_hid_keyboard_handle_report(const uint8_t report[8])
{
    if (!report) {
        return;
    }

    const uint8_t modifiers = report[0];
    const uint8_t *keys = report + 2;
    hid_keyboard_seen = true;

    if (keys[0] == 1 || keys[0] == 2 || keys[0] == 3) {
        log_warn("usb", "HID keyboard rollover/error report ignored");
        return;
    }

    for (uint8_t bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        bool was = (hid_last_modifiers & mask) != 0;
        bool now = (modifiers & mask) != 0;
        if (was == now) {
            continue;
        }
        uint8_t set1 = 0;
        bool extended = false;
        if (hid_modifier_to_set1(bit, &set1, &extended)) {
            hid_emit_set1(set1, extended, !now);
        }
    }

    for (size_t i = 0; i < 6; i++) {
        uint8_t usage = hid_last_keys[i];
        if (usage && !hid_has_key(keys, usage)) {
            uint8_t set1 = 0;
            bool extended = false;
            if (hid_usage_to_set1(usage, &set1, &extended)) {
                hid_emit_set1(set1, extended, true);
            }
        }
    }

    for (size_t i = 0; i < 6; i++) {
        uint8_t usage = keys[i];
        if (usage && !hid_has_key(hid_last_keys, usage)) {
            uint8_t set1 = 0;
            bool extended = false;
            if (hid_usage_to_set1(usage, &set1, &extended)) {
                hid_emit_set1(set1, extended, false);
            }
        }
    }

    hid_last_modifiers = modifiers;
    memcpy(hid_last_keys, keys, sizeof(hid_last_keys));
}

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
        log_info("usb", "USB HID keyboard decoder is available; host-controller polling is pending");
        log_info("usb", "PS/2 keyboard emulation remains available when firmware provides it");
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
    if (hid_keyboard_seen) {
        return true;
    }
    for (size_t i = 0; i < controller_count; i++) {
        if (controllers[i].hid_ready) {
            return true;
        }
    }
    return false;
}
