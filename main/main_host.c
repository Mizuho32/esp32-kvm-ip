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

#include "hid_forwarder.h"
#include "mruby_filter.h"
#include "mruby_webui.h"
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

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Must run before wifi_manager_start() - the script's `hostname "..."`
    // call (if any) needs to have been evaluated before the netif is set
    // up. See mds/usb_hid/2026-08-28_mruby_filter_route.md's hostname
    // section: one firmware image is meant to run on multiple boards now,
    // so there's no single compile-time hostname constant anymore -
    // mruby_filter_hostname() returns NULL (leave the chip's own default
    // hostname alone) unless the loaded script set one.
    mruby_filter_init();

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
    ESP_ERROR_CHECK(wifi_manager_start(WIFI_SSID, WIFI_PASSWORD, mruby_filter_hostname()));

    mruby_filter_resolve_udp_sinks();
    mruby_filter_start_net_source();

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
#else
    (void)typec_capable;
#endif

    status_led_set(true);
#if !HOST_MINIMAL_TEST
    ESP_LOGI(TAG, "System ready - forwarding USB HID input to %s:%d", KVM_TARGET_HOST, UDP_PORT);
#else
    ESP_LOGI(TAG, "HOST_MINIMAL_TEST ready - bridge_task running standalone");
#endif
}
