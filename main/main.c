#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include <stdio.h>

#include "hid_task.h"
#include "network_task.h"
#include "protocol.h"
#include "status_led.h"
#include "usb_descriptors.h"
#include "wifi_credentials.h"
#include "wifi_manager.h"

#define TAG "MAIN"

QueueHandle_t hid_event_queue;

void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 KVM starting...");

    status_led_init();

    // 1. Initialize NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // 2. Connect WiFi (blocks until IP obtained or retries exhausted) -
    // this board's whole job depends on the network (UDP-only input),
    // so unlike the Host role it keeps blocking here before doing
    // anything else - see wifi_manager.h.
    ESP_ERROR_CHECK(wifi_manager_start(WIFI_SSID, WIFI_PASSWORD, WIFI_HOSTNAME));
    esp_err_t wifi_ret = wifi_manager_wait_connected();
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed (0x%x). Restarting in 5s...", wifi_ret);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    // 3. Disable WiFi Modem Sleep (eliminates ~200ms lag on first packet)
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

    // 4. Initialize TinyUSB
    usb_descriptors_init();
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = TINYUSB_TASK_DEFAULT(),
        .descriptor = {
            .device            = &s_device_descriptor,
            .string            = s_string_descriptors,
            .string_count      = usb_string_descriptor_count,
            .full_speed_config = s_configuration_descriptor,
        },
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "TinyUSB initialized");

    hid_event_queue = xQueueCreate(32, sizeof(hid_event_t));
    if (hid_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create HID event queue");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    // 5. Start tasks on separate cores
    BaseType_t xRet;
    xRet = xTaskCreatePinnedToCore(network_task, "network_task", 4096, NULL, 5, NULL, 0);
    configASSERT(xRet == pdPASS);

    xRet = xTaskCreatePinnedToCore(hid_task, "hid_task", 4096, NULL, 6, NULL, 1);
    configASSERT(xRet == pdPASS);

    status_led_set(true);
    ESP_LOGI(TAG, "System ready - listening for UDP on port %d", UDP_PORT);
}
