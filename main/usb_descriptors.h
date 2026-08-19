#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdint.h>
#include "tusb.h"
#include "class/hid/hid_device.h"

// Two separate HID interfaces (no Report ID), each Boot Protocol capable.
// TinyUSB HID class-driver "instance" numbers are assigned in interface
// order, so these double as both interface numbers and instance indices.
#define ITF_NUM_KEYBOARD 0
#define ITF_NUM_MOUSE    1
#define ITF_NUM_TOTAL    2

// hid_keyboard_report_t / hid_mouse_report_t (from class/hid/hid_device.h)
// are TinyUSB's own "Standard HID Boot Protocol Report" structs - using
// them directly (with no Report ID) means the exact same bytes sent are
// valid in both Boot Protocol and Report Protocol mode, so no runtime
// protocol-switch handling is required.

// USB descriptors (defined in usb_descriptors.c)
extern tusb_desc_device_t s_device_descriptor;
extern const uint8_t s_configuration_descriptor[];
extern const char *s_string_descriptors[];
extern uint8_t usb_string_descriptor_count;

/**
 * Build string descriptor table and device descriptor indices
 * based on Kconfig. Must be called before tinyusb_driver_install().
 */
void usb_descriptors_init(void);

#endif
