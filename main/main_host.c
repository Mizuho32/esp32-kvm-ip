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

    // Must run before wifi_manager_init() - the script's `hostname "..."`
    // call (if any) needs to have been evaluated before the netif is set
    // up. See mds/usb_hid/2026-08-28_mruby_filter_route.md's hostname
    // section: one firmware image is meant to run on multiple boards now,
    // so there's no single compile-time hostname constant anymore -
    // mruby_filter_hostname() returns NULL (leave the chip's own default
    // hostname alone) unless the loaded script set one.
    mruby_filter_init();

#if !HOST_MINIMAL_TEST
    esp_err_t wifi_ret = wifi_manager_init(WIFI_SSID, WIFI_PASSWORD, mruby_filter_hostname());
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed (0x%x). Restarting in 5s...", wifi_ret);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

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
    // mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md. RP2040 bridge is
    // tried first (currently the preferred backend - see the RP2040 doc
    // for why), then MAX3421E, then native OTG as the last-resort
    // fallback. Whichever of the first two backends is used, native OTG
    // is deliberately left unused so it's free for the USB Device
    // (type-c) output path (Phase2); native OTG fallback can't offer
    // that (it's already busy being the Host input). All backends funnel
    // into the same hid_forwarder.c pipeline either way.
    bool typec_capable = false;
    if (usb_host_rp2040_bridge_probe()) {
        ESP_LOGI(TAG, "RP2040 bridge detected - using UART USB Host backend");
        if (usb_host_rp2040_bridge_task_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start RP2040 bridge task. Restarting in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        typec_capable = true;
    } else if (usb_host_max3421_probe()) {
        ESP_LOGI(TAG, "MAX3421E detected - using SPI USB Host backend");
        if (usb_host_max3421_task_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start MAX3421 USB host task. Restarting in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        typec_capable = true;
    } else {
        ESP_LOGI(TAG, "No RP2040 bridge or MAX3421E detected - using native OTG USB Host backend");
        if (usb_host_task_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start USB host task. Restarting in 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
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
