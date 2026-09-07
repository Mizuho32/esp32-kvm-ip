#ifndef BLE_HID_DEVICE_H
#define BLE_HID_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST). Advertises this board as a BLE HID
 * (HOGP - HID over GATT Profile) combo keyboard+mouse+Consumer-Control
 * device, so a physical USB keyboard/mouse read locally (usb_host_task.c/
 * usb_host_max3421.c/usb_host_rp2040_bridge.c) can be forwarded straight
 * to a target PC over Bluetooth Low Energy - no wired USB connection to
 * that PC needed at all. Independent of (and can run at the same time
 * as) the existing wired Type-C output (usb_device_typec.c) - see
 * mds/usb_hid/2026-09-07_ble_hid_sink_plan.md.
 *
 * Report layout mirrors usb_descriptors.c's Report Protocol formats
 * exactly (same byte order/fields), just with an explicit Report ID
 * byte prefixed per HOGP's single-service, multi-report-characteristic
 * convention (USB doesn't need one - each HID function has its own
 * interface/endpoint there instead):
 *   - Report ID 1: keyboard, Boot Protocol layout (modifiers, reserved,
 *     6 keycodes) - 8 bytes, same as usb_descriptors.h's
 *     hid_keyboard_report_t.
 *   - Report ID 2: mouse (5 buttons + 3 padding bits, 16-bit X/Y, 8-bit
 *     wheel, 8-bit AC Pan) - 7 bytes, same fields as
 *     usb_descriptors.c's Report Protocol mouse descriptor.
 *   - Report ID 3: Consumer Control (media keys), 16-bit usage code - 2
 *     bytes, same as TinyUSB's TUD_HID_REPORT_DESC_CONSUMER().
 *
 * Pairing is Just Works (no PIN/passkey, no on-device UI at all - see
 * esp_hid_gap.c's esp_hid_ble_gap_adv_init()) - entirely driven from the
 * target PC's own OS Bluetooth settings. Bonding is persisted to NVS by
 * NimBLE's own store (ble_store_config_init(), called internally), so a
 * previously-paired PC reconnects automatically on later boots without
 * re-pairing.
 */

/**
 * Brings up the BT controller + NimBLE host + HOGP HID service and
 * starts advertising. Call once, only if the loaded mruby script
 * declared a `:ble` sink (mruby_filter_ble_sink_declared()) - this pulls
 * in the NimBLE stack, so boards that never use it pay nothing.
 *
 * @return ESP_OK on success.
 */
esp_err_t ble_hid_device_start(void);

/**
 * @return true if a central (the target PC) is currently connected over
 *         BLE right now. false before ble_hid_device_start(), while
 *         advertising/unpaired, or if ble_hid_device_start() was never
 *         called at all.
 */
bool ble_hid_device_connected(void);

/** Sends a keyboard report (Report ID 1). No-op if not connected. */
void ble_hid_device_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

/** Sends a mouse report (Report ID 2). No-op if not connected. */
void ble_hid_device_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/** Sends a Consumer Control report (Report ID 3). No-op if not connected. */
void ble_hid_device_consumer_report(uint16_t usage_id);

/**
 * Forgets the current bond (if any) and disconnects, so a different PC
 * can pair next. Exposed for the WebUI's "Unpair" button
 * (mruby_webui.c) - see mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's
 * ペアリング section. Safe to call even if nothing is bonded/connected.
 */
void ble_hid_device_unpair(void);

#endif
