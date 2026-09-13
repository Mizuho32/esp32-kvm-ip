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

#include "ble_pair_slots.h"
#include "mruby_filter.h"
#include "nimble_hidd_fork.h"
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

// True only once the NimBLE host has actually finished its sync handshake
// with the controller (ESP_HIDD_START_EVENT, driven by
// nimble_hidd_fork.c's nimble_host_synced() -> ble_hs_cfg.sync_cb) -
// distinct from s_started, which flips true as soon as
// ble_hid_device_start() finishes *launching* the stack, well before that
// handshake completes. ble_pair_slots.c needs this distinction:
// ble_pair_switch()/ble_pair_new() called right after `ble_toggle true`
// (s_started already true) could still race ahead of sync and send an
// advertising HCI command too early - real-hardware symptom, "ble_hs_hci_cmd_send_buf
// rc=22" / "error setting advertisement data" - see
// mds/usb_hid/2026-09-12_ble_multi_pair.md's follow-up.
static bool s_host_synced;

// Set right before ble_hid_device_stop() forces the stack down, cleared
// again the next time ble_hid_device_start() brings it back up - tells
// hidd_event_callback()'s ESP_HIDD_DISCONNECT_EVENT case to do nothing.
// Needed because ble_hid_device_stop() deliberately no longer calls
// esp_hidd_dev_deinit() (see its own doc comment) - which means the GAP
// event listener esp_hidd's nimble_hidd.c registered is never explicitly
// unregistered (that call lived inside the very deinit path we now skip,
// and nimble_gap_event_listener is nimble_hidd.c's own static - nothing
// outside that file can unregister it). ble_hs_deinit()'s forced
// disconnect (of whatever's still connected) still reaches that
// listener and still gets posted up to this callback as an ordinary
// ESP_HIDD_DISCONNECT_EVENT - asynchronously, via esp_event's own task,
// so it can (and on real hardware did) arrive *after*
// ble_hid_device_stop() has already returned and torn the whole NimBLE
// host/controller down. Without this flag the handler would try to
// resume WiFi and re-advertise into a stack that no longer exists -
// real-hardware crash, LoadProhibited inside ble_hs_is_enabled(), see
// mds/usb_hid/2026-09-11_ble_reconnect_holdoff.md's follow-up.
static bool s_ignore_disconnect_events;

// Forward-declared: defined alongside ble_hid_device_mouse_report() below,
// but hidd_event_callback() (right below) needs to call it on connect/
// disconnect.
static void ble_hid_mouse_pending_reset(void);

// Created once, on the first ever ble_hid_device_start(), and kept
// forever (never vSemaphoreDelete()'d) - mirrors debug_stream.c's
// immortal-queue design (mds/usb_hid/2026-09-10_mruby_debug_stream.md):
// a small, fixed-cost primitive simplest left alone, with only the
// actual heavy BLE/NimBLE stack itself toggled by
// ble_hid_device_start()/_stop() (mds/usb_hid/2026-09-11_ble_dynamic_enable.md).
static SemaphoreHandle_t s_nimble_host_stopped_sem;

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
        ESP_LOGI(TAG, "started");
        s_host_synced = true;
        // ble_pair_slots.c decides whether/who to advertise to (directed
        // at whichever slot was last active, or a switch()/new() call
        // that came in too early and had to wait for this - or stay idle
        // if neither) - see mds/usb_hid/2026-09-12_ble_multi_pair.md.
        ble_pair_slots_resume_on_start();
        status_led_set_ble_advertising(ble_pair_current_slot() >= 0);
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

        if (s_ignore_disconnect_events) {
            // ble_hid_device_stop() is tearing (or has already torn) the
            // whole stack down - see s_ignore_disconnect_events' doc
            // comment above for why this event still arrives anyway, and
            // why touching WiFi/advertising here would be unsafe.
            ESP_LOGI(TAG, "disconnect is part of an intentional ble_toggle(false) shutdown - not resuming");
            break;
        }

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

        // Whether/who to re-advertise to now is entirely ble_pair_slots.c's
        // call (esp_hid_gap.c's nimble_hid_gap_event() already invoked
        // ble_pair_slots_on_disconnect() for this same disconnect, from
        // its own listener - it has the disconnect reason and, unlike
        // this event, the peer's address too). Just reflect whatever slot
        // state that left behind on the status LED.
        status_led_set_ble_advertising(ble_pair_current_slot() >= 0);
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

    s_host_synced = false; // flips true again once ESP_HIDD_START_EVENT actually arrives

    // A fresh stack is coming up - any stray disconnect event left over
    // from a previous ble_hid_device_stop() is done mattering, and this
    // start's own events need to be handled normally again. See
    // s_ignore_disconnect_events' doc comment.
    s_ignore_disconnect_events = false;

    // Created once (first ever start) and kept forever - see its
    // declaration above for why.
    if (s_nimble_host_stopped_sem == NULL) {
        s_nimble_host_stopped_sem = xSemaphoreCreateBinary();
        if (s_nimble_host_stopped_sem == NULL) {
            ESP_LOGE(TAG, "xSemaphoreCreateBinary (nimble host stop signal) failed");
            return ESP_ERR_NO_MEM;
        }
    }
    // Diagnostic only (mds/usb_hid/2026-09-13_ble_idle_crash.md): a
    // real-hardware crash (assert failed: ble_hs_init ble_hs.c:995, from
    // ble_gattc_init() returning BLE_HS_ENOMEM) hit esp_nimble_init() a
    // couple of ble_toggle cycles into a session left idle for ~9 hours -
    // consistent with internal-RAM heap pressure that built up somewhere
    // (BLE-cycle-related or not) rather than a single dramatic leak, but
    // unconfirmed without numbers from an actual occurrence. Logging free/
    // largest-block here on every start (and again at the end of stop()
    // below) costs nothing and turns the next occurrence into hard
    // evidence either way.
    ESP_LOGI(TAG, "start: internal free=%" PRIu32 " largest block=%" PRIu32,
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

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

    // kvm_ble_hidd_dev_init() (nimble_hidd_fork.c/.h), not the generic
    // esp_hidd_dev_init(..., ESP_HID_TRANSPORT_BLE, ...) - see that fork's
    // file header comment for why. Same call shape/return semantics
    // otherwise, and everything downstream (esp_hidd_dev_input_set() in
    // the report-sending functions below, esp_hidd_dev_deinit() in
    // ble_hid_device_stop()) works completely unchanged.
    ret = kvm_ble_hidd_dev_init(&s_hid_config, hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "kvm_ble_hidd_dev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_store_config_init(); // NVS-backed bonding persistence
    nimble_port_freertos_init(nimble_host_task);

    s_started = true;
    ESP_LOGI(TAG, "BLE HID device starting as '%s'", s_hid_config.device_name);
    return ESP_OK;
}

// Symmetric teardown of everything ble_hid_device_start() brought up,
// short of s_nimble_host_stopped_sem itself (see its declaration comment
// - it stays allocated forever so a later ble_hid_device_start() never
// has to recreate it). Called by mruby_filter.c's `ble_toggle false`
// (see mds/usb_hid/2026-09-11_ble_dynamic_enable.md) - blocks its caller
// for roughly as long as the BT controller/NimBLE host take to actually
// shut down (not instantaneous, unlike most of this file's other calls -
// see that doc's note on what this means for whichever dispatch task
// calls it holding mruby_filter.c's s_mrb_mutex meanwhile).
esp_err_t ble_hid_device_stop(void)
{
    if (!s_started) {
        return ESP_OK; // already stopped - idempotent, same as debug_stream_stop()
    }

    // Snapshot before anything below changes it - needed for the WiFi
    // resume just below, since (see that block's comment) the normal
    // ESP_HIDD_DISCONNECT_EVENT path that would otherwise have done this
    // never fires for a stop() while still connected.
    bool was_connected = s_connected;

    // From here on, any ESP_HIDD_DISCONNECT_EVENT - including one
    // triggered as a side effect of the forced disconnect inside
    // ble_hs_deinit() further down, possibly delivered well after this
    // function returns (see s_ignore_disconnect_events' doc comment) -
    // must not touch WiFi or advertising.
    s_ignore_disconnect_events = true;

    // Not currently advertising is a normal case (already connected, or
    // never started advertising this cycle) - ble_gap_adv_stop()'s
    // BLE_HS_EALREADY-ish "wasn't advertising" return is expected and
    // harmless, not logged as an error.
    ble_gap_adv_stop();

    // esp_hidd_dev_deinit() -> nimble_hidd_fork.c's (forked) HID device
    // profile: disconnects the current connection if any, then stops the
    // GATT server and deinits the HID/DIS/BAS/SPS/GATT/GAP service layers -
    // the actual symmetric counterpart to kvm_ble_hidd_dev_init() above.
    // This used to be the *original*, unforked esp_hid nimble_hidd.c, which
    // crashed real hardware here (LoadProhibited in ble_gatts_free_mem()) -
    // its own internal ble_gatts_stop() call collided with the *other*
    // ble_gatts_stop() call esp_hid_gap_deinit() below makes via
    // esp_nimble_deinit()->ble_hs_deinit(), a redundant double-teardown of
    // the same NimBLE GATT internals. nimble_hidd_fork.c's
    // nimble_hid_stop_gatts() no longer makes that call itself - see its
    // own comment - so esp_hid_gap_deinit()'s is now the only one. See
    // mds/usb_hid/2026-09-11_ble_reconnect_holdoff.md's follow-up for the
    // full history (including the addr2line-resolved crash trace) and why
    // forking was chosen over just leaving the controller/host resident
    // across ble_toggle(false) cycles.
    if (s_hid_dev != NULL) {
        esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
    }

    // Normally it's hidd_event_callback()'s ESP_HIDD_DISCONNECT_EVENT case
    // (above) that calls wifi_manager_resume() when a
    // ble_wifi_off_while_connected-suspended connection ends - but that
    // event never arrives for *this* disconnect: nimble_hidd_fork.c's
    // nimble_hid_stop_gatts() (just run via esp_hidd_dev_deinit() above)
    // unregisters its own GAP event listener *before* calling
    // ble_gap_terminate() to drop the connection, so its own
    // nimble_hid_gap_event() - the thing that would otherwise post
    // ESP_HIDD_DISCONNECT_EVENT up to hidd_event_callback() - never runs
    // for it. (esp_hid_gap.c's *separate* raw GAP listener still logs its
    // own "disconnect; reason=..." line regardless - that's a different
    // listener, unaffected.) Real-hardware symptom this caused: LED still
    // showing advertising/on, but WiFi never actually came back (no ping)
    // after ble_toggle(false) while ble_wifi_off_while_connected was
    // active and a peer was connected. Do the resume here instead, since
    // this is the one place guaranteed to run exactly once per stop().
    if (was_connected && mruby_filter_ble_wifi_off_while_connected()) {
        esp_err_t err = wifi_manager_resume();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_manager_resume() failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "ble_wifi_off_while_connected: WiFi resumed (ble_toggle false while connected)");
        }
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
    s_host_synced = false;
    // Whatever the LED was doing (blinking - waiting on a connection or
    // still mid-ble_pair_new() pairing) is meaningless now that the whole
    // stack is gone - nothing left advertising to reflect. Missing this
    // used to leave the LED stuck blinking forever after a `ble_toggle
    // false` called mid-advertise - real-hardware repro, see
    // mds/usb_hid/2026-09-12_ble_multi_pair.md's follow-up.
    status_led_set_ble_advertising(false);
    ESP_LOGI(TAG, "BLE HID device stopped");
    // Paired with the entry log in ble_hid_device_start() - see that log's
    // comment. Logged after teardown so it reflects whatever this cycle's
    // shutdown gave back (or didn't).
    ESP_LOGI(TAG, "stop: internal free=%" PRIu32 " largest block=%" PRIu32,
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ret;
}

bool ble_hid_device_connected(void)
{
    return s_started && s_connected;
}

bool ble_hid_device_started(void)
{
    return s_started;
}

bool ble_hid_device_ready(void)
{
    return s_started && s_host_synced;
}

void ble_hid_device_disconnect_current(void)
{
    if (s_hid_dev != NULL && s_connected) {
        kvm_ble_hidd_dev_disconnect(s_hid_dev);
    }
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
    // Wipes *every* stored bond (NimBLE's own ble_store_config NVS store)
    // and every ble_pair_slots.c slot's remembered peer alike - the WebUI's
    // "Unpair" button is a full reset, not a per-slot operation (use
    // ble_pair_new() from the script for that instead - see
    // mds/usb_hid/2026-09-12_ble_multi_pair.md). Disconnects whatever's
    // currently connected first so the reset is immediate rather than
    // waiting for that peer to eventually drop on its own.
    ble_hid_device_disconnect_current();
    ble_pair_slots_forget_all();
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "unpaired (all bonds and slots cleared)");
    }
}
