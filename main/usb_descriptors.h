#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdbool.h>
#include <stdint.h>
#include "tusb.h"
#include "class/hid/hid_device.h"

// Three HID interfaces. Keyboard and Mouse are Boot Protocol capable;
// Consumer Control is Report-protocol only (BIOS never needs media keys).
// TinyUSB HID class-driver "instance" numbers are assigned in interface
// order, so these double as both interface numbers and instance indices.
#define ITF_NUM_KEYBOARD 0
#define ITF_NUM_MOUSE    1
#define ITF_NUM_CONSUMER 2
#define ITF_NUM_TOTAL    3

// ── Report layouts ───────────────────────────────────────────────
//
// The Report Descriptor is only consulted by the host while it is in
// Report Protocol mode (the default, and what every OS switches back to
// once it loads a generic HID driver). While a host is in Boot Protocol
// mode (BIOS/bootloaders) it ignores the descriptor entirely and expects
// the hard-coded legacy layout instead - so the same physical interface
// can legitimately send different bytes depending on the current mode,
// checked via tud_hid_n_get_protocol(instance):
//
//   - HID_PROTOCOL_BOOT   -> hid_keyboard_report_t / hid_mouse_report_t
//                            (TinyUSB's own fixed Boot Protocol structs,
//                            from class/hid/hid_device.h)
//   - HID_PROTOCOL_REPORT -> keyboard uses the same struct (it was never
//                            extended beyond the boot layout); mouse uses
//                            mouse_report_t below (full 16-bit relative
//                            movement, matches the original KMChris design)
//
// Keyboard has no separate Report-mode struct: hid_keyboard_report_t is
// used for both modes since it was already the full feature set.

typedef struct __attribute__((packed)) {
    uint8_t buttons;    // bit0=Left, bit1=Right, bit2=Middle, bit3=Back, bit4=Forward
    int16_t x;          // Relative movement X (-32767 … +32767)
    int16_t y;          // Relative movement Y
    int8_t  wheel;      // Vertical scroll  (-127 … +127)
    int8_t  pan;        // Horizontal scroll (-127 … +127)
} mouse_report_t;       // 7 bytes - Report Protocol mode

_Static_assert(sizeof(mouse_report_t) == 7, "Mouse report must be 7 bytes");

typedef struct __attribute__((packed)) {
    uint16_t usage_id;  // Consumer Usage ID (0x0C page), 0 = release
} consumer_report_t;    // 2 bytes

_Static_assert(sizeof(consumer_report_t) == 2, "Consumer report must be 2 bytes");

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

/**
 * @return true if the PC currently has this USB Device link suspended
 *         (bus-level SUSPEND state - no SOF traffic for >3ms - tracked
 *         via tud_suspend_cb()/tud_resume_cb() in usb_descriptors.c).
 *         Distinct from VBUS/power presence: a suspended PC still
 *         supplies VBUS, so this is the actual "PC went to sleep" signal
 *         - see mds/usb_hid/2026-8-30_Sleep.md. Always false if
 *         tinyusb_driver_install() was never called (e.g. Host role with
 *         no type-c output).
 */
bool usb_device_suspended(void);

#endif
