#ifndef TNU_USB_H
#define TNU_USB_H

#include <tnu/types.h>

#define USB_CONTROLLER_MAX 8

enum usb_controller_type {
    USB_CONTROLLER_UHCI,
    USB_CONTROLLER_OHCI,
    USB_CONTROLLER_EHCI,
    USB_CONTROLLER_XHCI,
    USB_CONTROLLER_UNKNOWN,
};

struct usb_controller_info {
    enum usb_controller_type type;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t prog_if;
    const char *driver;
    bool hid_ready;
    bool host_ready;
    bool inventory_only;
};

void usb_init(void);
size_t usb_controller_count(void);
const struct usb_controller_info *usb_controller_get(size_t index);
const char *usb_controller_type_name(enum usb_controller_type type);
bool usb_hid_keyboard_ready(void);
void usb_hid_keyboard_handle_report(const uint8_t report[8]);

#endif
