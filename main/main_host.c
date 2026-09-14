// Host role (KVM_ROLE=HOST) entry point. See mds/usb_hid/2026-08-21_usb_host.md.
//
// Reads a physical USB keyboard/mouse plugged into this board's USB OTG
// port (in USB Host mode) and forwards it over WiFi/UDP to a Device-role
// esp32-kvm-ip board (main.c/hid_task.c), which is unchanged and needs no
// awareness of this Host role at all.

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_hid_device.h"
#include "crash_report.h"
#include "heap_monitor.h"
#include "hid_forwarder.h"
#include "mruby_filter.h"
#include "mruby_webui.h"
#include "power_manager.h"
#include "protocol.h"
#include "status_led.h"
#include "usb_device_typec.h"
#include "usb_host_max3421.h"
#include "usb_host_rp2040_bridge.h"
#include "usb_host_task.h"
#include "wifi_credentials.h"
#include "wifi_manager.h"

#define TAG "MAIN_HOST"

// When 1: skip WiFi, hid_forwarder (UDP socket), and the type-c USB
// Device output entirely - only the RP2040 bridge backend runs (and,
// if usb_host_rp2040_bridge.c's own BRIDGE_MINIMAL_TEST is also set,
// only its UART-parsing bridge_task, not even dispatch_task). Isolates
// whether WiFi/lwIP or the TinyUSB Device stack are what's starving
// bridge_task for tens of ms at a time
// (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md) - disabling just
// their console logging didn't change the symptom, so this removes the
// subsystems themselves rather than just their logging.
#define HOST_MINIMAL_TEST 0

// EXPERIMENTAL performance-isolation toggle (mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
// follow-up) - a BLE-focused sibling of HOST_MINIMAL_TEST above: skips
// WiFi/mruby_filter_init()/WebUI/type-c/power_manager entirely and starts
// BLE HID unconditionally (no mruby DSL running, so there's no `:ble`
// sink declaration to gate on) alongside whichever USB Host backend is
// probed. Only the RP2040 bridge (UART USB Host) is exercised here - see
// try_rp2040_bridge() below - since that's this project's actual target
// hardware backend; MAX3421E/native-OTG paths are skipped for simplicity.
// Must be flipped together with hid_forwarder.c's matching
// HOST_BLE_ONLY_TEST, which is what actually routes every keyboard/
// mouse/consumer report straight to ble_hid_device_*_report(), bypassing
// mruby/UDP/type-c per-report (this toggle alone only controls what
// app_main() brings up at boot, not the per-report dispatch path).
#define HOST_BLE_ONLY_TEST 0

// Each returns true if that backend was actually detected/started (and
// hard-restarts on a detected-but-failed-to-start error, same as before -
// these are just the probe+start pairs factored out so both the
// script-driven loop and the pure-C fallback below can share them
// without duplicating the restart-on-failure logic). See
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
static bool try_rp2040_bridge(void)
{
    if (!usb_host_rp2040_bridge_probe()) {
        return false;
    }
    ESP_LOGI(TAG, "RP2040 bridge detected - using UART USB Host backend");
    if (usb_host_rp2040_bridge_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RP2040 bridge task. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    return true;
}

static bool try_max3421(void)
{
    if (!usb_host_max3421_probe()) {
        return false;
    }
    ESP_LOGI(TAG, "MAX3421E detected - using SPI USB Host backend");
    if (usb_host_max3421_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MAX3421 USB host task. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    return true;
}

static void start_native_otg_host(void)
{
    ESP_LOGI(TAG, "Using native OTG USB Host backend");
    if (usb_host_task_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB host task. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 KVM (Host role) starting...");

    status_led_init();
    heap_monitor_init(); // see mds/usb_hid/2026-09-13_ble_idle_crash.md - diagnostic only

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // See mds/usb_hid/2026-09-13_crash_reporting.md - reads back and
    // clears any coredump left by a crash on the *previous* boot,
    // regardless of which #if branch below this boot takes. Needs NVS
    // (just initialized above) but nothing else.
    crash_report_init();

#if HOST_BLE_ONLY_TEST
    // See this toggle's definition above - BLE + RP2040 bridge only,
    // nothing else. NVS is still needed (NimBLE's bond store,
    // CONFIG_BT_NIMBLE_NVS_PERSIST - see mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
    // follow-up), which is why it's initialized above regardless.
    ESP_LOGW(TAG, "HOST_BLE_ONLY_TEST: BLE + RP2040 bridge only - no WiFi/mruby/WebUI/type-c");
    if (ble_hid_device_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE HID device. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    if (!try_rp2040_bridge()) {
        ESP_LOGE(TAG, "RP2040 bridge not detected - HOST_BLE_ONLY_TEST requires it. Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    status_led_set(true);
    ESP_LOGI(TAG, "HOST_BLE_ONLY_TEST ready - BLE + RP2040 bridge running standalone");
    return;
#endif

    // Must run before wifi_manager_start() - the script's `hostname "..."`
    // call (if any) needs to have been evaluated before the netif is set
    // up. See mds/usb_hid/2026-08-28_mruby_filter_route.md's hostname
    // section: one firmware image is meant to run on multiple boards now,
    // so there's no single compile-time hostname constant anymore -
    // mruby_filter_hostname() returns NULL (leave the chip's own default
    // hostname alone) unless the loaded script set one.
    mruby_filter_init();

    // No WiFi/lwIP dependency (unlike mruby_filter_resolve_udp_sinks()/
    // _start_net_source() below) - start capturing UART RX as early as
    // possible. See mds/usb_hid/2026-09-14_uart_bridge.md.
    mruby_filter_resolve_uart_bridges();

#if !HOST_MINIMAL_TEST
    // Does not block for an actual AP connection (unlike Device role's
    // main.c) - local USB Host -> type-c input has no WiFi dependency at
    // all, so it shouldn't be held up by however long association takes.
    // WiFi keeps connecting in the background regardless (wifi_manager.c's
    // event_handler auto-retries forever); IP_EVENT_STA_GOT_IP logs
    // "WiFi connected" whenever it actually happens, and everything below
    // this point only needs the TCP/IP thread wifi_manager_start()
    // already brought up (getaddrinfo()/socket()/bind() - see
    // mruby_filter.h and mds/usb_hid/2026-08-29_mruby_phase1_impl.md), not
    // a completed AP association - see mds/usb_hid/2026-08-30_mruby_wifi_deferred.md.
    // Credentials come from the wifi_cred partition
    // (bin/upload_wifi_credentials.py), not a compile-time constant - see
    // wifi_manager_load_credentials(). Hostname is unrelated - that's the
    // mruby script's `hostname` call above, not this partition.
    char wifi_ssid[33], wifi_password[64];
    if (!wifi_manager_load_credentials(wifi_ssid, sizeof(wifi_ssid),
                                        wifi_password, sizeof(wifi_password), NULL, 0)) {
        ESP_LOGE(TAG, "No WiFi credentials - upload with bin/upload_wifi_credentials.py --port ... --ssid ...");
    }
    ESP_ERROR_CHECK(wifi_manager_start(wifi_ssid, wifi_password, mruby_filter_hostname()));

    // Only actually does anything if crash_report_init() above found a
    // new crash this boot *and* the script set crash_notify_url - see
    // that function's doc comment (crash_report.h).
    crash_report_notify_after_wifi();

    mruby_filter_resolve_udp_sinks();
    mruby_filter_start_net_source();

    // BLE HID output (mds/usb_hid/2026-09-07_ble_hid_sink_plan.md) - only
    // pulled in if the script actually declared a `:ble` sink (see
    // mruby_filter_ble_sink_declared()'s doc comment). Independent of
    // WiFi/type-c - can run alongside either or both. Skipped entirely if
    // the script opted into `ble_dynamic true` - see
    // mruby_filter_ble_dynamic()'s doc comment
    // (mds/usb_hid/2026-09-11_ble_dynamic_enable.md): the script is then
    // responsible for calling `ble_toggle true` itself, whenever it wants.
    if (mruby_filter_ble_sink_declared() && !mruby_filter_ble_dynamic()) {
        if (ble_hid_device_start() != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start BLE HID device (not fatal - continuing without it)");
        }
    }

    // Phase 2 (mds/usb_hid/2026-08-30_mruby_phase2_webui.md): browser-based
    // script editing, no serial/parttool.py round-trip needed.
    mruby_webui_start();

    if (hid_forwarder_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init HID forwarder (UDP socket). Restarting in 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
#else
    ESP_LOGW(TAG, "HOST_MINIMAL_TEST: skipping WiFi/hid_forwarder/type-c entirely");
#endif

    // Exactly one Host backend runs, never more than one - see
    // usb_host_max3421.h, usb_host_rp2040_bridge.h and
    // mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md /
    // mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md. All backends
    // funnel into the same hid_forwarder.c pipeline either way.
    //
    // Which backend(s) to try, and in what order, is controlled entirely
    // by the loaded mruby script (`usb_host_backends(*syms)` - see
    // mruby_filter.h and mds/usb_hid/2026-08-29_mruby_phase1_impl.md) so
    // a board that only wants network (:udp source) input, for example,
    // can leave native OTG free for type-c device output instead of
    // claiming it as a (useless to it) local USB Host input backend.
    // If mruby isn't active at all (VM failed to open, or both the
    // uploaded script and the embedded default.rb failed to parse),
    // mruby_filter_host_backend_count() is 0 and this falls back to the
    // original, fully hardcoded probe order (RP2040 bridge, then
    // MAX3421E, then native OTG) unconditionally - the "fall back to
    // pure C" path.
    bool typec_capable = false;
    int backend_count = mruby_filter_host_backend_count();
    if (backend_count > 0) {
        bool started = false;
        for (int i = 0; i < backend_count && !started; i++) {
            switch (mruby_filter_host_backend_at(i)) {
            case MRUBY_HOST_BACKEND_RP2040_BRIDGE:
                if (try_rp2040_bridge()) {
                    typec_capable = true;
                    started = true;
                }
                break;
            case MRUBY_HOST_BACKEND_MAX3421:
                if (try_max3421()) {
                    typec_capable = true;
                    started = true;
                }
                break;
            case MRUBY_HOST_BACKEND_NATIVE_OTG:
                start_native_otg_host();
                started = true; // typec_capable stays false - native OTG is claimed for host input
                break;
            }
        }
        if (!started) {
            ESP_LOGI(TAG, "No USB Host backend detected/configured - native OTG free for type-c device output only");
            typec_capable = true;
        }
    } else {
        if (try_rp2040_bridge()) {
            typec_capable = true;
        } else if (try_max3421()) {
            typec_capable = true;
        } else {
            start_native_otg_host();
        }
    }

#if !HOST_MINIMAL_TEST
    if (typec_capable) {
        // Native OTG is free (whichever backend above is in use isn't
        // using it) - bring it up as a type-c USB Device output (Phase2).
        // Not fatal if it fails: hid_forwarder.c falls back to UDP-only
        // when usb_device_typec_connected() is false.
        if (usb_device_typec_start() != ESP_OK) {
            ESP_LOGW(TAG, "type-c USB Device output failed to start (not fatal - continuing UDP-only)");
        }
    }
    // Harmless to start even when !typec_capable (usb_device_typec_start()
    // was never called, so tud_suspend_cb/tud_resume_cb simply never fire
    // on this board - the task just blocks forever waiting for the first
    // notification) - see power_manager.h. Skipped entirely under
    // HOST_MINIMAL_TEST since wifi_manager_start() never ran there either,
    // so there'd be nothing for wifi_manager_suspend()/_resume() to act on.
    power_manager_init();
#else
    (void)typec_capable;
#endif

#if !HOST_MINIMAL_TEST
    // No unconditional status_led_set(true) here - wifi_manager.c's
    // event_handler() now owns this LED (blinks while connecting for the
    // first time, solid once connected - see status_led_set_blinking()),
    // and forcing it on here regardless of that state would cut the
    // blink short on every boot, defeating the point of it.
    ESP_LOGI(TAG, "System ready - forwarding USB HID input to %s:%d", KVM_TARGET_HOST, UDP_PORT);
#else
    // wifi_manager_start() never ran under HOST_MINIMAL_TEST (see
    // above), so nothing else drives the LED - just show "ready" directly.
    status_led_set(true);
    ESP_LOGI(TAG, "HOST_MINIMAL_TEST ready - bridge_task running standalone");
#endif
}
