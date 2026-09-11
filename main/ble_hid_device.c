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

#include "esp_coexist.h"
#include "esp_heap_caps.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "esp_timer.h"

#include "mruby_filter.h"
#include "status_led.h"
#include "wifi_manager.h"

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
#define BLE_HID_REPORT_ID_SYSCTL   4

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

    // ── System Control (Report ID 4): Power Down/Sleep/Wake Up/menu
    //    navigation/restart, 1 byte - the reported value IS the raw HID
    //    Usage ID directly (0x81-0x8F, see usb_descriptors.c's
    //    SYSTEM_CONTROL_USAGE_MIN/MAX and its doc comment for why this
    //    range and why Usage Minimum/Maximum == Logical Minimum/Maximum
    //    rather than TinyUSB's stock 3-value Array template), 0 = idle/
    //    none - mirrors usb_descriptors.c's hand-written descriptor
    //    exactly (same Usage Page/range/field width). Unlike the other
    //    three reports above, nothing in this project ever *reads* a
    //    physical device that produces this - only an mruby script
    //    calling `system_control :sleep, ...` ever sends it
    //    (mruby_filter.c) - see
    //    mds/usb_hid/2026-09-10_system_control_sleep.md.
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x80,        //   Usage (System Control)
    0xA1, 0x01,        //   Collection (Application)
    0x85, BLE_HID_REPORT_ID_SYSCTL,
    0x16, 0x81, 0x00,  //     Logical Minimum (0x0081) - 2-byte form, 0x81 exceeds signed 8-bit range
    0x26, 0x8F, 0x00,  //     Logical Maximum (0x008F)
    0x19, 0x81,        //     Usage Minimum (0x81) - Usage Min/Max read as unsigned, 1 byte is enough
    0x29, 0x8F,        //     Usage Maximum (0x8F)
    0x75, 0x08,        //     Report Size (8)
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

// Forward-declared: defined alongside ble_hid_device_mouse_report() below,
// but hidd_event_callback() (right below) needs to call it on connect/
// disconnect.
static void ble_hid_mouse_pending_reset(void);

// How long to hold off re-advertising after a *deliberate* disconnect
// (see hidd_event_callback()'s ESP_HIDD_DISCONNECT_EVENT case) before
// letting the peer reconnect again. Most OSes auto-reconnect to a
// bonded/trusted HID device the instant they see it advertising again,
// so re-advertising immediately after the user explicitly disconnected
// (e.g. from the PC's own Bluetooth settings) just gets it silently
// reconnected within moments - defeating the point of having
// disconnected at all, and (paired with mruby_filter_ble_wifi_off_while_connected())
// leaving no real window to use WiFi/WebUI. See
// mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up. 30s is long
// enough to actually do something over WiFi, short enough that it
// doesn't feel "stuck" if a normal reconnect was wanted instead.
#define BLE_REDISCONNECT_HOLDOFF_US (30 * 1000 * 1000)

// Both created once, on the first ever ble_hid_device_start(), and kept
// forever (never esp_timer_delete()'d / vSemaphoreDelete()'d) - mirrors
// debug_stream.c's immortal-queue design (mds/usb_hid/2026-09-10_mruby_debug_stream.md):
// small, fixed-cost primitives that are simplest left alone, with only
// the actual heavy BLE/NimBLE stack itself toggled by
// ble_hid_device_start()/_stop() (mds/usb_hid/2026-09-11_ble_dynamic_enable.md).
static esp_timer_handle_t s_readvertise_timer;
static SemaphoreHandle_t s_nimble_host_stopped_sem;

static void readvertise_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "resuming advertising after deliberate-disconnect holdoff");
    esp_hid_ble_gap_adv_start();
    status_led_set_ble_advertising(true);
}

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
        status_led_set_ble_advertising(true);
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        status_led_set_ble_advertising(false); // no longer waiting - back to plain solid-on
        ble_hid_mouse_pending_reset(); // discard any backlog from before this connection existed
        if (mruby_filter_ble_wifi_off_while_connected()) {
            // See mruby_filter_ble_wifi_off_while_connected()'s doc
            // comment (mruby_filter.h) for why this is opt-in, not
            // unconditional. Not fatal if it fails - BLE HID keeps
            // working either way, just without this optimization.
            esp_err_t err = wifi_manager_suspend();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi_manager_suspend() failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "ble_wifi_off_while_connected: WiFi stopped while BLE is connected");
            }
        }
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
        ESP_LOGI(TAG, "disconnected (reason %d)", param->disconnect.reason);
        s_connected = false;
        ble_hid_mouse_pending_reset(); // don't deliver a stale backlog as one big jump on reconnect
        if (mruby_filter_ble_wifi_off_while_connected()) {
            esp_err_t err = wifi_manager_resume();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi_manager_resume() failed: %s", esp_err_to_name(err));
            } else {
                // Internal-SRAM stats here are the "reconnect just kicked
                // off" bookend - esp_wifi_start() returning doesn't mean
                // the STA reconnect/DHCP handshake is done yet (that's
                // still async), so this is the *start* of whatever
                // transient dip wifi_manager.c's IP_EVENT_STA_GOT_IP log
                // captures the end of. See that log's comment and
                // mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up
                // (a WebUI save's "parse failed (out of memory?)" seen
                // shortly after this point in real testing).
                ESP_LOGI(TAG, "ble_wifi_off_while_connected: WiFi resumed - WebUI reachable again "
                         "(internal free=%" PRIu32 " largest block=%" PRIu32 ")",
                         (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }
        }

        // NimBLE reports HCI status/reason codes as
        // BLE_HS_ERR_HCI_BASE(0x200) + the raw HCI error (see ble_hs.h) -
        // 0x13 (BLE_ERR_REM_USER_CONN_TERM, "Remote User Terminated
        // Connection") is what either side's Bluetooth stack sends for a
        // *deliberate* disconnect (e.g. the user disconnecting from the
        // PC's own Bluetooth settings), as opposed to something like 0x08
        // "Connection Timeout" (radio range/interference - an involuntary
        // drop). Re-advertising immediately after a deliberate disconnect
        // just invites most OSes' own auto-reconnect-to-bonded-HID-device
        // policy to reconnect within moments - defeating the point of
        // having disconnected at all, and (paired with
        // ble_wifi_off_while_connected above) leaving no real window to
        // use WiFi/WebUI. See mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
        // follow-up. Hold off in that case only - an involuntary drop
        // still re-advertises immediately, so a real out-of-range
        // reconnect isn't delayed.
        if (param->disconnect.reason == (BLE_HS_ERR_HCI_BASE + BLE_ERR_REM_USER_CONN_TERM) &&
            s_readvertise_timer != NULL) {
            ESP_LOGI(TAG, "deliberate disconnect - holding off re-advertising for %ds",
                     (int)(BLE_REDISCONNECT_HOLDOFF_US / 1000000));
            // Not advertising during the holdoff - plain solid-on (or off,
            // if WiFi itself isn't up) is the honest state to show; the
            // pattern resumes from readvertise_timer_cb() once advertising
            // actually restarts.
            status_led_set_ble_advertising(false);
            esp_timer_stop(s_readvertise_timer); // no-op if not already running
            esp_timer_start_once(s_readvertise_timer, BLE_REDISCONNECT_HOLDOFF_US);
        } else {
            ESP_LOGI(TAG, "resuming advertising");
            esp_hid_ble_gap_adv_start();
            status_led_set_ble_advertising(true);
        }
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
    // Returns once nimble_port_stop() is called - ble_hid_device_stop()
    // below is now the one call site that does that (previously never,
    // when this stack was only ever started once at boot and left
    // running for the process's whole lifetime). Giving
    // s_nimble_host_stopped_sem here, right after nimble_port_run()
    // actually returns but *before* nimble_port_freertos_deinit() - that
    // call self-deletes this very task (vTaskDelete(NULL) internally), so
    // nothing after it in this function ever runs; it's what lets
    // ble_hid_device_stop()'s caller block until the host task has
    // genuinely exited before touching the BT controller itself. See
    // mds/usb_hid/2026-09-11_ble_dynamic_enable.md.
    nimble_port_run();
    if (s_nimble_host_stopped_sem != NULL) {
        xSemaphoreGive(s_nimble_host_stopped_sem);
    }
    nimble_port_freertos_deinit();
}

esp_err_t ble_hid_device_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    // Created once (first ever start) and kept forever - see their
    // declarations above for why.
    if (s_readvertise_timer == NULL) {
        const esp_timer_create_args_t readvertise_timer_args = {
            .callback = readvertise_timer_cb,
            .name = "ble_readv",
        };
        esp_err_t timer_ret = esp_timer_create(&readvertise_timer_args, &s_readvertise_timer);
        if (timer_ret != ESP_OK) {
            // Not fatal - just means a deliberate disconnect (see
            // hidd_event_callback()) falls back to immediate re-advertising
            // instead of holding off, same as before this feature existed.
            ESP_LOGW(TAG, "esp_timer_create (re-advertise holdoff) failed: %s", esp_err_to_name(timer_ret));
            s_readvertise_timer = NULL;
        }
    }
    if (s_nimble_host_stopped_sem == NULL) {
        s_nimble_host_stopped_sem = xSemaphoreCreateBinary();
        if (s_nimble_host_stopped_sem == NULL) {
            ESP_LOGE(TAG, "xSemaphoreCreateBinary (nimble host stop signal) failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = esp_hid_gap_init(HIDD_BLE_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ESP32-S3's WiFi and BT share one 2.4GHz radio - esp_coex arbitrates
    // airtime between them, and defaults to favoring WiFi. Real-hardware
    // testing (mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up)
    // isolated this as the actual cause of BLE mouse motion feeling much
    // choppier whenever WiFi is associated/active, vs. smooth with WiFi
    // fully disabled - not mruby's per-report dispatch overhead (ruled
    // out separately) or BLE's own connection-interval floor alone.
    // Explicitly biasing the arbiter toward BT only once BLE is actually
    // in use (not globally at boot) keeps the :typec/:udp-only case
    // (no :ble sink declared, this function never runs) on the default
    // WiFi-favoring behavior, unaffected.
    ret = esp_coex_preference_set(ESP_COEX_PREFER_BT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_coex_preference_set(BT) failed: %s (continuing anyway - not fatal)",
                 esp_err_to_name(ret));
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

// Symmetric teardown of everything ble_hid_device_start() brought up,
// short of s_readvertise_timer/s_nimble_host_stopped_sem themselves (see
// their declaration comment - those two stay allocated forever so a
// later ble_hid_device_start() never has to recreate them). Called by
// mruby_filter.c's `ble_enable false` (see
// mds/usb_hid/2026-09-11_ble_dynamic_enable.md) - blocks its caller for
// roughly as long as the BT controller/NimBLE host take to actually shut
// down (not instantaneous, unlike most of this file's other calls - see
// that doc's note on what this means for whichever dispatch task calls
// it holding mruby_filter.c's s_mrb_mutex meanwhile).
esp_err_t ble_hid_device_stop(void)
{
    if (!s_started) {
        return ESP_OK; // already stopped - idempotent, same as debug_stream_stop()
    }

    // Cancel a pending deliberate-disconnect re-advertise holdoff (if
    // any) rather than deleting the timer - it stays alive for next time
    // (see its declaration comment). Left pending, it would fire
    // readvertise_timer_cb() *after* the teardown below, calling
    // esp_hid_ble_gap_adv_start() against an already-deinitialized BT
    // controller - undefined behavior, not just a harmless no-op.
    if (s_readvertise_timer != NULL) {
        esp_timer_stop(s_readvertise_timer); // no-op (ESP_ERR_INVALID_STATE, ignored) if not pending
    }

    // Not currently advertising is a normal case (already connected, or
    // never started advertising this cycle) - ble_gap_adv_stop()'s
    // BLE_HS_EALREADY-ish "wasn't advertising" return is expected and
    // harmless, not logged as an error.
    ble_gap_adv_stop();

    // esp_hidd_dev_deinit() -> esp_hid's nimble_hidd.c's
    // nimble_hid_stop_gatts() (read from ESP-IDF's own source, not
    // assumed) already disconnects the current connection if any, then
    // stops the GATT server and deinits the HID/DIS/BAS/SPS/GATT/GAP
    // service layers - the actual symmetric counterpart to
    // esp_hidd_dev_init() above.
    if (s_hid_dev != NULL) {
        esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
    }

    // nimble_port_stop() only *requests* the NimBLE host event loop to
    // exit - it does not block until it actually has. Waiting on
    // s_nimble_host_stopped_sem (given by nimble_host_task() right after
    // nimble_port_run() returns) is what makes this call synchronous -
    // esp_hid_gap_deinit() below disables/deinits the BT controller
    // itself, which must not happen while the NimBLE host task might
    // still be mid-shutdown.
    nimble_port_stop();
    xSemaphoreTake(s_nimble_host_stopped_sem, portMAX_DELAY);

    esp_err_t ret = esp_hid_gap_deinit(); // esp_nimble_deinit() + BT controller disable+deinit
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_hid_gap_deinit failed: %s (continuing anyway)", esp_err_to_name(ret));
    }

    // Restore the default WiFi-favoring coex arbitration now that BLE
    // isn't in use - mirrors ble_hid_device_start()'s own comment on why
    // it biases toward BT in the first place.
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    s_started = false;
    s_connected = false;
    ESP_LOGI(TAG, "BLE HID device stopped");
    return ret;
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

// BLE can carry at most one GATT notification per connection event (see
// esp_hid_gap.c's BLE_GAP_EVENT_CONNECT handler, which now requests the
// shortest interval the spec allows - 7.5ms - but that's still a hard
// per-connection-event cap, not a queue). A physical mouse sampling
// faster than that occasionally hits esp_hidd_dev_input_set() before the
// previous notification has actually gone out, which fails outright
// (ble_gatts_notify_custom() -> ble_att_clt_tx_notify() doesn't queue -
// see mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up). dx/dy/
// wheel/pan are relative deltas, so a failed send doesn't have to mean
// lost motion: accumulate it into the next attempt instead of dropping
// it on the floor. buttons is deliberately *not* accumulated here - it's
// the current absolute button state (not a delta), so the next real
// sample already carries the right value regardless of what got dropped.
static int32_t s_pending_dx, s_pending_dy, s_pending_wheel, s_pending_pan;

// Called on connect/disconnect (hidd_event_callback() above) so a
// backlog accumulated before a connection existed - or before a *new*
// one after a drop - never gets delivered as one large surprise jump.
static void ble_hid_mouse_pending_reset(void)
{
    s_pending_dx = s_pending_dy = s_pending_wheel = s_pending_pan = 0;
}

void ble_hid_device_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    if (!ble_hid_device_connected()) {
        return;
    }

    int32_t total_dx    = s_pending_dx    + dx;
    int32_t total_dy    = s_pending_dy    + dy;
    int32_t total_wheel = s_pending_wheel + wheel;
    int32_t total_pan   = s_pending_pan   + pan;

    // Clamp to the report map's actual field widths (16-bit signed for
    // dx/dy, 8-bit signed for wheel/pan - see ble_hid_device.h) instead
    // of letting a large backlog silently wrap. A backlog this big is
    // already well past what one report can carry regardless; clamping
    // loses only the excess beyond the field's range, not the whole delta.
    if (total_dx > INT16_MAX) total_dx = INT16_MAX;
    else if (total_dx < INT16_MIN) total_dx = INT16_MIN;
    if (total_dy > INT16_MAX) total_dy = INT16_MAX;
    else if (total_dy < INT16_MIN) total_dy = INT16_MIN;
    if (total_wheel > INT8_MAX) total_wheel = INT8_MAX;
    else if (total_wheel < INT8_MIN) total_wheel = INT8_MIN;
    if (total_pan > INT8_MAX) total_pan = INT8_MAX;
    else if (total_pan < INT8_MIN) total_pan = INT8_MIN;

    // Little-endian, matching the report map's byte order (and this
    // MCU's own native endianness, so a straight memcpy is correct) -
    // see ble_hid_device.h.
    uint8_t buf[7];
    buf[0] = buttons;
    int16_t out_dx = (int16_t)total_dx;
    int16_t out_dy = (int16_t)total_dy;
    memcpy(&buf[1], &out_dx, 2);
    memcpy(&buf[3], &out_dy, 2);
    buf[5] = (uint8_t)(int8_t)total_wheel;
    buf[6] = (uint8_t)(int8_t)total_pan;

    // esp_hidd_dev_input_set()'s return value used to be silently
    // discarded - a per-call success log was tried temporarily during an
    // earlier investigation and dropped again: this fires on every
    // physical mouse-move event, and this project has already measured
    // blocking UART console output as a real source of input latency on
    // this same hot path (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md).
    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, BLE_HID_REPORT_ID_MOUSE, buf, sizeof(buf));
    if (err != ESP_OK) {
        // Couldn't fit this connection event - hold onto the accumulated
        // deltas (this call's own dx/dy/wheel/pan included) for the next
        // attempt instead of dropping the motion.
        s_pending_dx    = total_dx;
        s_pending_dy    = total_dy;
        s_pending_wheel = total_wheel;
        s_pending_pan   = total_pan;
        ESP_LOGW(TAG, "mouse report send failed: %s (buttons=%u dx=%d dy=%d) - accumulating for retry",
                 esp_err_to_name(err), buttons, dx, dy);
    } else {
        ble_hid_mouse_pending_reset();
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

void ble_hid_device_system_control_report(uint16_t usage_id)
{
    if (!ble_hid_device_connected()) {
        return;
    }
    // usage_id IS the wire value directly (see this file's report map
    // comment above and usb_device_typec_system_control_report()'s
    // mirrored comment) - no separate mapping step.
    uint8_t buf[1] = { (uint8_t)usage_id };
    esp_hidd_dev_input_set(s_hid_dev, 0, BLE_HID_REPORT_ID_SYSCTL, buf, sizeof(buf));
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
