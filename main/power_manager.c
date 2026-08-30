#include "power_manager.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "usb_descriptors.h"
#include "wifi_manager.h"

#define TAG "POWER_MGR"

#if CONFIG_USB_SUSPEND_LIGHT_SLEEP_ENABLE

// ESP32-S3 has no USB-bus wake source for light sleep (no
// esp_sleep_enable_usb_wakeup() in this IDF - checked against
// esp_hw_support/include/esp_sleep.h and soc_caps.h, neither has one for
// this chip, unlike the dedicated hardware UART wakeup has), so the
// original design here cycled short timer-woken esp_light_sleep_start()
// slices, polling usb_device_suspended() between them.
//
// Real-hardware testing (2026-08-30) showed this actually calling
// esp_light_sleep_start() corrupts the USB peripheral's state on this
// chip - the confirmed ESP32-S3 40MHz-crystal light-sleep USB bug (see
// CONFIG_USB_SUSPEND_ACTUAL_LIGHT_SLEEP's help in Kconfig.projbuild):
// tud_resume_cb fired far too early (~600ms into a 2s slice) followed
// immediately by another spurious tud_suspend_cb with a *different*
// remote_wakeup_en value than the real suspend had - not something a real
// PC suspend/resume/suspend could produce within tens of milliseconds -
// and the board never recovered. So CONFIG_USB_SUSPEND_ACTUAL_LIGHT_SLEEP
// now defaults off: with it off, this just waits for the resume
// notification directly (tinyusb's task stays fully scheduled throughout
// - nothing sleeps - so tud_resume_cb fires immediately and normally, no
// polling/corruption risk, but also no CPU-sleep power saving beyond the
// WiFi stop). Leave the option available for further experimentation once/
// if the underlying peripheral issue has a real fix.
#define LIGHT_SLEEP_SLICE_US   (2000 * 1000)
#define POST_WAKE_SETTLE_MS    100

// Host role's mruby_filter.c (KVM_ROLE=HOST only) may define this to let a
// script opt out per-deployment (`usb_suspend_wifi_sleep false` - see
// mruby_filter.h). Declared weak and bodyless for the same reason as
// usb_descriptors.c's power_manager_on_usb_suspend_changed() lookup below
// it: KVM_ROLE=DEVICE builds don't compile mruby_filter.c at all, so this
// resolves to NULL there and the null check just falls through to "always
// enabled".
extern bool mruby_filter_usb_suspend_wifi_sleep_enabled(void) __attribute__((weak));

static bool suspend_action_enabled(void)
{
    if (mruby_filter_usb_suspend_wifi_sleep_enabled) {
        return mruby_filter_usb_suspend_wifi_sleep_enabled();
    }
    return true;
}

static TaskHandle_t s_task_handle;

static void power_manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        // Blocks here whenever not suspended. A resume notification that
        // arrives while already awake (not-suspended) just falls through
        // the `if` below and loops back to waiting - usb_device_suspended()
        // is the actual source of truth, not the notification itself.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!usb_device_suspended()) {
            continue;
        }
        if (!suspend_action_enabled()) {
            ESP_LOGI(TAG, "USB suspended, but usb_suspend_wifi_sleep is off - staying awake");
            continue;
        }

        ESP_LOGI(TAG, "PC suspended - stopping WiFi, cycling light sleep");
        status_led_set(false);

        esp_err_t err = wifi_manager_suspend();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi_manager_suspend() failed: %s", esp_err_to_name(err));
        }

#if CONFIG_USB_SUSPEND_ACTUAL_LIGHT_SLEEP
        while (usb_device_suspended()) {
            esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_SLICE_US);
            esp_light_sleep_start();
            // Give tinyusb's task a scheduling window to run tud_task()
            // and notice the bus is active again (tud_resume_cb) before
            // re-checking/re-sleeping.
            vTaskDelay(pdMS_TO_TICKS(POST_WAKE_SETTLE_MS));
        }
#else
        // No actual sleep - just wait for the notification
        // power_manager_on_usb_suspend_changed() sends on the next
        // tud_resume_cb (tinyusb's task keeps running normally the whole
        // time, so that fires as soon as the bus really is active again).
        do {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } while (usb_device_suspended());
#endif

        ESP_LOGI(TAG, "PC resumed - restarting WiFi");
        wifi_manager_resume();
        status_led_set(true);
    }
}

void power_manager_init(void)
{
    BaseType_t ok = xTaskCreate(power_manager_task, "power_mgr", 3072, NULL, 4, &s_task_handle);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create power_manager_task");
    }
}

void power_manager_on_usb_suspend_changed(bool suspended)
{
    (void)suspended;
    if (s_task_handle != NULL) {
        xTaskNotifyGive(s_task_handle);
    }
}

#else

void power_manager_init(void)
{
}

void power_manager_on_usb_suspend_changed(bool suspended)
{
    (void)suspended;
}

#endif
