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
 *   - Report ID 4: System Control (Power Down/Sleep/Wake Up), 1 byte -
 *     same 2-bit Array field encoding as TinyUSB's
 *     TUD_HID_REPORT_DESC_SYSTEM_CONTROL() (usb_descriptors.c). Unlike
 *     the other three, no physical device this project reads from ever
 *     produces this - see mds/usb_hid/2026-09-10_system_control_sleep.md.
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
 * Tears down everything ble_hid_device_start() brought up (disconnects
 * if connected, stops advertising, deinits the HID/GATT service layer,
 * stops the NimBLE host task, disables+deinits the BT controller) - a
 * later ble_hid_device_start() call brings it all back. A no-op
 * returning ESP_OK if not currently started. Exposed for
 * mruby_filter.c's `ble_enable false` DSL call
 * (mds/usb_hid/2026-09-11_ble_dynamic_enable.md) - lets a script turn
 * BLE off again at runtime (e.g. via a keyboard shortcut), not just on.
 * Blocks its caller for roughly as long as the underlying stack actually
 * takes to shut down (not instantaneous) - see that doc for what this
 * means when called from mruby's dispatch path.
 *
 * @return ESP_OK on success (or if already stopped).
 */
esp_err_t ble_hid_device_stop(void);

/**
 * @return true if a central (the target PC) is currently connected over
 *         BLE right now. false before ble_hid_device_start(), while
 *         advertising/unpaired, or if ble_hid_device_start() was never
 *         called at all.
 */
bool ble_hid_device_connected(void);

/**
 * @return true while the BLE/NimBLE stack itself is up (between a
 *         successful ble_hid_device_start() and the next
 *         ble_hid_device_stop()) - regardless of whether a peer is
 *         actually connected right now. Distinct from
 *         ble_hid_device_connected() above, which additionally requires
 *         a live connection. Exposed for mruby_filter.c's `ble_enable`
 *         DSL call's no-argument toggle form (flips based on the real
 *         state rather than a script-tracked guess) - see
 *         mds/usb_hid/2026-09-11_ble_dynamic_enable.md's follow-up.
 */
bool ble_hid_device_started(void);

/** Sends a keyboard report (Report ID 1). No-op if not connected. */
void ble_hid_device_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

/** Sends a mouse report (Report ID 2). No-op if not connected. */
void ble_hid_device_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/** Sends a Consumer Control report (Report ID 3). No-op if not connected. */
void ble_hid_device_consumer_report(uint16_t usage_id);

/** Sends a System Control report (Report ID 4) - raw HID Usage ID
 * 0x81/0x82/0x83 (Power Down/Sleep/Wake Up), 0 = idle/release, same
 * convention as ble_hid_device_consumer_report()'s usage_id. No-op if not
 * connected. */
void ble_hid_device_system_control_report(uint16_t usage_id);

/**
 * Forgets the current bond (if any) and disconnects, so a different PC
 * can pair next. Exposed for the WebUI's "Unpair" button
 * (mruby_webui.c) - see mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's
 * ペアリング section. Safe to call even if nothing is bonded/connected.
 */
void ble_hid_device_unpair(void);

#endif
