#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#include <stdbool.h>
#include <stdint.h>
#include "tusb.h"
#include "class/hid/hid_device.h"

// Four HID interfaces. Keyboard and Mouse are Boot Protocol capable;
// Consumer Control and System Control are Report-protocol only (BIOS
// never needs media keys or a software Sleep button). TinyUSB HID
// class-driver "instance" numbers are assigned in interface order, so
// these double as both interface numbers and instance indices.
#define ITF_NUM_KEYBOARD 0
#define ITF_NUM_MOUSE    1
#define ITF_NUM_CONSUMER 2
#define ITF_NUM_SYSCTL   3
#define ITF_NUM_TOTAL    4

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

// System Control (Generic Desktop page 0x01, Power Down/Sleep/Wake Up) -
// see mds/usb_hid/2026-09-10_system_control_sleep.md. Unlike the other
// three interfaces, no physical device this project reads from ever
// produces this - it only ever originates from an mruby script calling
// `system_control :sleep, ...` (mruby_filter.c), typically from a
// :keyboard pipeline branch() watching for a key combo a real keyboard
// doesn't have a dedicated key for.
typedef struct __attribute__((packed)) {
    // TUD_HID_REPORT_DESC_SYSTEM_CONTROL()'s 2-bit Array field value, NOT
    // the raw HID Usage ID - 0 = idle, 1 = Power Down, 2 = Sleep, 3 = Wake
    // Up (usage_id_to_array_value() in usb_device_typec.c/ble_hid_device.c
    // does that mapping right before building this struct; every other
    // layer - protocol.h's udp_packet_t/hid_event_t, mruby_filter.c's
    // system_control() - carries the actual 0x81/0x82/0x83 usage ID
    // instead, same convention as consumer_report_t's usage_id above).
    uint8_t value;
} system_control_report_t; // 1 byte

_Static_assert(sizeof(system_control_report_t) == 1, "System Control report must be 1 byte");

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

/**
 * Manually forces usb_device_suspended() to true and notifies
 * power_manager.c, exactly as if tud_suspend_cb() had just fired. Covers
 * the case that callback can't: a PC that was *already* suspended before
 * this board booted/connected to it, which never produces a bus SUSPEND
 * transition for tinyusb to notice. Used by mruby_webui.c's "Sleep now"
 * button. Clears back to false normally, via a real tud_resume_cb() once
 * the PC actually resumes - if it wasn't really suspended, this stays
 * stuck true (WiFi off) until that happens or the board is reset, so it's
 * meant as a deliberate user action, not something to call speculatively.
 */
void usb_device_force_suspended(void);

#endif
