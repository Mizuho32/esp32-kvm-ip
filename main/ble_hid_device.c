// See ble_hid_device.h and mds/usb_hid/2026-09-07_ble_hid_sink_plan.md.
//
// esp_hid_gap.c (vendored from ESP-IDF's examples/bluetooth/esp_hid_device)
// owns the generic NimBLE/GAP/bonding boilerplate; this file owns the
// combo keyboard+mouse+Consumer-Control HID report maps and the
// project-specific send/connect/unpair API (usb_device_typec.h's
// signatures, mirrored deliberately - see mruby_filter.c's
// send_*_to_sink() dispatch, which calls both the same way).

#include "ble_hid_device.h"

#include <inttypes.h>
#include <string.h>

#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "BLE_HID";

// ═══════════════════════════════════════════════════════════════════
//  HID REPORT MAP - one combined raw map, three top-level Application
//  collections (esp_hid_common.c's esp_hid_parse_report_map() walks
//  consecutive top-level collections in the same blob, each with its
//  own Report ID - this is the standard way a single HOGP HID Service/
//  Report Map characteristic carries multiple report kinds). Byte
//  layout of each report is deliberately identical to
//  usb_descriptors.c's Report Protocol formats - see ble_hid_device.h.
// ═══════════════════════════════════════════════════════════════════

#define BLE_HID_REPORT_ID_KEYBOARD 1
#define BLE_HID_REPORT_ID_MOUSE    2
#define BLE_HID_REPORT_ID_CONSUMER 3

static const uint8_t s_ble_hid_report_map[] = {
    // ── Keyboard (Report ID 1): Boot Protocol layout, 8 bytes
    //    (modifiers, reserved, 6 keycodes) - same as
    //    usb_descriptors.h's hid_keyboard_report_t. No Output report
    //    (LEDs) - not needed for this project's own use.
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x06,        //   Usage (Keyboard)
    0xA1, 0x01,        //   Collection (Application)
    0x85, BLE_HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,        //     Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //     Usage Minimum (0xE0)
    0x29, 0xE7,        //     Usage Maximum (0xE7)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x08,        //     Report Count (8) - modifier bits
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x08,        //     Report Size (8) - reserved byte
    0x81, 0x01,        //     Input (Const,Array,Abs)
    0x95, 0x06,        //     Report Count (6) - keycodes
    0x75, 0x08,        //     Report Size (8)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x65,        //     Logical Maximum (101)
    0x05, 0x07,        //     Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //     Usage Minimum (0)
    0x29, 0x65,        //     Usage Maximum (101)
    0x81, 0x00,        //     Input (Data,Array,Abs)
    0xC0,              //   End Collection

    // ── Mouse (Report ID 2): 5 buttons + 3 padding bits, 16-bit X/Y,
    //    8-bit wheel, 8-bit AC Pan - 7 bytes. Same fields as
    //    usb_descriptors.c's s_hid_report_descriptor_mouse.
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x02,        //   Usage (Mouse)
    0xA1, 0x01,        //   Collection (Application)
    0x09, 0x01,        //     Usage (Pointer)
    0xA1, 0x00,        //     Collection (Physical)
    0x85, BLE_HID_REPORT_ID_MOUSE,
    0x05, 0x09,        //       Usage Page (Button)
    0x19, 0x01,        //       Usage Minimum (Button 1)
    0x29, 0x05,        //       Usage Maximum (Button 5)
    0x15, 0x00,        //       Logical Minimum (0)
    0x25, 0x01,        //       Logical Maximum (1)
    0x95, 0x05,        //       Report Count (5)
    0x75, 0x01,        //       Report Size (1)
    0x81, 0x02,        //       Input (Data,Var,Abs)
    0x95, 0x01,        //       Report Count (1)
    0x75, 0x03,        //       Report Size (3) - padding
    0x81, 0x01,        //       Input (Const,Array,Abs)
    0x05, 0x01,        //       Usage Page (Generic Desktop)
    0x09, 0x30,        //       Usage (X)
    0x09, 0x31,        //       Usage (Y)
    0x16, 0x01, 0x80,  //       Logical Minimum (-32767)
    0x26, 0xFF, 0x7F,  //       Logical Maximum (32767)
    0x75, 0x10,        //       Report Size (16)
    0x95, 0x02,        //       Report Count (2)
    0x81, 0x06,        //       Input (Data,Var,Rel)
    0x09, 0x38,        //       Usage (Wheel)
    0x15, 0x81,        //       Logical Minimum (-127)
    0x25, 0x7F,        //       Logical Maximum (127)
    0x75, 0x08,        //       Report Size (8)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x06,        //       Input (Data,Var,Rel)
    0x05, 0x0C,        //       Usage Page (Consumer)
    0x0A, 0x38, 0x02,  //       Usage (AC Pan)
    0x15, 0x81,        //       Logical Minimum (-127)
    0x25, 0x7F,        //       Logical Maximum (127)
    0x75, 0x08,        //       Report Size (8)
    0x95, 0x01,        //       Report Count (1)
    0x81, 0x06,        //       Input (Data,Var,Rel)
    0xC0,              //     End Collection
    0xC0,              //   End Collection

    // ── Consumer Control (Report ID 3): 16-bit usage code, 2 bytes -
    //    same range as TinyUSB's TUD_HID_REPORT_DESC_CONSUMER().
    0x05, 0x0C,        //   Usage Page (Consumer)
    0x09, 0x01,        //   Usage (Consumer Control)
    0xA1, 0x01,        //   Collection (Application)
    0x85, BLE_HID_REPORT_ID_CONSUMER,
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x03,  //     Logical Maximum (0x3FF)
    0x19, 0x00,        //     Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //     Usage Maximum (0x3FF)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x00,        //     Input (Data,Array,Abs)
    0xC0,              //   End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_ble_hid_report_map,
        .len  = sizeof(s_ble_hid_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id         = 0x16C0, // Van Ooijen Technische Informatica's shared VID, same as TinyUSB's default (usb_descriptors.c) - no dedicated VID for this hobby project.
    .product_id        = 0x05DF,
    .version            = 0x0100,
    .device_name        = "Wireless USBHID",
    .manufacturer_name  = "Wireless_USBHID project",
    .serial_number      = "1",
    .report_maps        = s_report_maps,
    .report_maps_len    = 1,
};

static esp_hidd_dev_t *s_hid_dev;
static bool s_connected;
static bool s_started;

// ═══════════════════════════════════════════════════════════════════
//  HID device event callback
// ═══════════════════════════════════════════════════════════════════

static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "started, advertising");
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        // Diagnostic only - see mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
        // follow-up notes on a suspected esp_hid quirk: keyboard/mouse
        // usages each get *two* auto-generated Input Report
        // characteristics (Report mode and Boot mode, both tagged with
        // the same Report ID) - esp_hidd_dev_input_set() routes to
        // whichever one matches the *current* protocol mode read back
        // from the peer, so if this ever logs BOOT, that's the likely
        // reason typec-equivalent reports go nowhere the OS is actually
        // listening.
        ESP_LOGI(TAG, "protocol mode[%u]: %s", param->protocol_mode.map_index,
                 param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "disconnected (reason %d) - resuming advertising",
                 param->disconnect.reason);
        s_connected = false;
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_STOP_EVENT:
        ESP_LOGI(TAG, "stopped");
        break;
    default:
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  NimBLE host task - mirrors the upstream example's
//  ble_hid_device_host_task()/ble_store_config_init() pair (esp_hid_gap.c
//  brings up the controller + NimBLE host config; this just runs it).
// ═══════════════════════════════════════════════════════════════════

void ble_store_config_init(void); // NimBLE's own bond store (store/config), no project header for it

// esp_hid_gap.c's nimble_hid_gap_event() (BLE_GAP_EVENT_ENC_CHANGE) calls
// this on encryption established - a hook the vendored example used to
// resume its own stdin-reading demo tasks (ble_hid_task_start_up()/
// _shut_down(), CONFIG_EXAMPLE_HID_DEVICE_ROLE-specific, never vendored
// here). We push reports directly from mruby_filter.c's dispatch instead
// of any such task, so this is just a no-op stub to satisfy the link -
// nothing to start up.
void ble_hid_task_start_up(void)
{
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); // returns only once nimble_port_stop() is called - never, for us
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_device_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t ret = esp_hid_gap_init(HIDD_BLE_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, s_hid_config.device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_ble_gap_adv_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_store_config_init(); // NVS-backed bonding persistence
    nimble_port_freertos_init(nimble_host_task);

    s_started = true;
    ESP_LOGI(TAG, "BLE HID device starting as '%s'", s_hid_config.device_name);
    return ESP_OK;
}

bool ble_hid_device_connected(void)
{
    return s_started && s_connected;
}

void ble_hid_device_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    if (!ble_hid_device_connected()) {
        return;
    }
    uint8_t buf[8] = { modifiers, 0 };
    memcpy(&buf[2], keycodes, 6);
    esp_hidd_dev_input_set(s_hid_dev, 0, BLE_HID_REPORT_ID_KEYBOARD, buf, sizeof(buf));
}

void ble_hid_device_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    if (!ble_hid_device_connected()) {
        return;
    }
    // Little-endian, matching the report map's byte order (and this
    // MCU's own native endianness, so a straight memcpy is correct) -
    // see ble_hid_device.h.
    uint8_t buf[7];
    buf[0] = buttons;
    memcpy(&buf[1], &dx, 2);
    memcpy(&buf[3], &dy, 2);
    buf[5] = (uint8_t)wheel;
    buf[6] = (uint8_t)pan;
    // esp_hidd_dev_input_set()'s return value used to be silently
    // discarded - the actual bug (hid_forwarder.c gating the whole mruby
    // pipeline, BLE sink included, behind usb_device_typec_connected() -
    // see mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up) has
    // since been found and fixed elsewhere, so this only logs on an
    // actual send failure now - a per-call success log was tried
    // temporarily during that investigation and dropped again: this
    // fires on every physical mouse-move event, and this project has
    // already measured blocking UART console output as a real source of
    // input latency on this same hot path (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md).
    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, BLE_HID_REPORT_ID_MOUSE, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mouse report send failed: %s (buttons=%u dx=%d dy=%d)",
                 esp_err_to_name(err), buttons, dx, dy);
    }
}

void ble_hid_device_consumer_report(uint16_t usage_id)
{
    if (!ble_hid_device_connected()) {
        return;
    }
    uint8_t buf[2];
    memcpy(buf, &usage_id, 2);
    esp_hidd_dev_input_set(s_hid_dev, 0, BLE_HID_REPORT_ID_CONSUMER, buf, sizeof(buf));
}

void ble_hid_device_unpair(void)
{
    if (!s_started) {
        return;
    }
    // MVP: wipe every stored bond (ble_store_config's NVS-backed store) -
    // fine for a single-PC-at-a-time device. Takes effect immediately for
    // future connection attempts; if a PC is connected *right now*, this
    // doesn't forcibly kick it (conn_handle isn't exposed by esp_hidd's
    // public event API) - it'll just fail to re-bond on its next
    // reconnect. Good enough for the MVP (mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's
    // "複数PCとのボンディング切り替えは保留" - revisit if this proves
    // annoying in practice.
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "unpaired (all bonds cleared)");
    }
}
