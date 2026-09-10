#ifndef USB_DEVICE_TYPEC_H
#define USB_DEVICE_TYPEC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST), MAX3421E backend only
 * (mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md's Phase2). Once
 * MAX3421E takes over Host duties over SPI, this board's own native
 * USB-OTG port is unused by anything else - this brings it up as a
 * plain USB HID device (reusing usb_descriptors.c/usb_descriptors.h, the
 * same 3-interface keyboard/mouse/Consumer Control descriptor set the
 * Device role uses) so it can be plugged directly into a PC and forward
 * MAX3421-read input there, alongside (or instead of, depending on
 * route_rules.h) the existing UDP path to a separate Device-role board.
 *
 * Goes through espressif/esp_tinyusb's tinyusb_driver_install() (same
 * call main.c's Device role uses) rather than hand-rolling PHY +
 * tud_rhport_init() the way usb_host_max3421.c does for the Host side:
 * esp_tinyusb's own tud_descriptor_device_cb()/_configuration_cb()/
 * _string_cb() (in its descriptors_control.c, always compiled in
 * regardless of KVM_ROLE - see main/idf_component.yml) are the only
 * definitions of those symbols, only get populated by this call, and
 * can't be duplicated - so this must be the one to call it. This is
 * independent of (and doesn't reopen) the tusb_config.h fight Host
 * role's MAX3421 side already had to route around - which tusb_config.h
 * wins is decided by components/tinyusb/CMakeLists.txt at build time,
 * regardless of which runtime entry point an app calls -
 * see usb_device_typec.c.
 *
 * Only meaningful when MAX3421E is present - see main_host.c, which only
 * calls usb_device_typec_start() in that branch. If MAX3421E is absent,
 * the native OTG port is already busy being the Host input (native OTG
 * fallback, usb_host_task.c) and can't simultaneously be a Device.
 */

/**
 * Starts the TinyUSB Device stack on rhport 0 (native OTG - see
 * CFG_TUSB_RHPORT0_MODE in components/tinyusb/host_config/tusb_config.h)
 * via tinyusb_driver_install(), including its background tud_task()
 * loop. Call once, only when MAX3421E was detected.
 *
 * @return ESP_OK on success.
 */
esp_err_t usb_device_typec_start(void);

/**
 * @return true if this port is actually enumerated by a PC right now
 *         (tud_mounted()) - false at boot, while unplugged, or if
 *         usb_device_typec_start() was never called (e.g. no MAX3421E).
 *         hid_forwarder.c uses this to decide UDP vs type-c routing.
 */
bool usb_device_typec_connected(void);

/** Sends a keyboard report (Boot Protocol layout, used for both Boot and
 * Report mode - see usb_descriptors.h) on the type-c HID keyboard
 * interface. No-op if not connected. */
void usb_device_typec_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

/** Sends a mouse report on the type-c HID mouse interface, picking Boot
 * or Report Protocol layout based on what the connected host currently
 * has selected (tud_hid_n_get_protocol()) - mirrors hid_task.c. No-op if
 * not connected. */
void usb_device_typec_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/** Sends a Consumer Control (media key) report on the type-c HID
 * Consumer interface. No-op if not connected. */
void usb_device_typec_consumer_report(uint16_t usage_id);

/** Sends a System Control report (Power Down/Sleep/Wake Up, raw HID
 * Usage ID 0x81/0x82/0x83, 0 = idle/release - see usb_descriptors.h's
 * system_control_report_t) on the type-c HID System Control interface.
 * No-op if not connected. */
void usb_device_typec_system_control_report(uint16_t usage_id);

#endif
