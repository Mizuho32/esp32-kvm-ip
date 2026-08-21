#include "usb_host_task.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "protocol.h"
#include "filter_rules.h"
#include "wifi_credentials.h"

#define TAG "USBHOST"

// HID host driver events (device connect/disconnect) are delivered from
// the driver's own background task and just get queued here; the actual
// per-report handling (hid_host_interface_callback below) runs directly
// on that background task instead, since it only does cheap work
// (filter + UDP send), matching how the official
// examples/peripherals/usb/host/hid example structures this.
static QueueHandle_t s_driver_event_queue;

static int s_sock = -1;
static struct sockaddr_in s_target_addr;
static uint32_t s_seq;

static bool resolve_target(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", UDP_PORT);

    int err = getaddrinfo(KVM_TARGET_HOST, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "Failed to resolve KVM_TARGET_HOST '%s': %d", KVM_TARGET_HOST, err);
        return false;
    }
    memcpy(&s_target_addr, res->ai_addr, sizeof(s_target_addr));
    freeaddrinfo(res);
    return true;
}

static void send_udp_packet(const udp_packet_t *pkt)
{
    if (s_sock < 0) {
        return;
    }
    sendto(s_sock, pkt, PACKET_SIZE, 0, (struct sockaddr *)&s_target_addr, sizeof(s_target_addr));
}

static void send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_KEYBOARD,
    };
    pkt.keyboard.modifiers = modifiers;
    pkt.keyboard.reserved  = 0;
    memcpy(pkt.keyboard.keycodes, keycodes, 6);
    send_udp_packet(&pkt);
}

static void send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_MOUSE,
    };
    pkt.mouse.buttons = buttons;
    pkt.mouse.dx      = dx;
    pkt.mouse.dy      = dy;
    pkt.mouse.wheel   = 0; // Boot Protocol mice don't report wheel/pan.
    pkt.mouse.pan     = 0;
    send_udp_packet(&pkt);
}

static void handle_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }
    const hid_keyboard_input_report_boot_t *report = (const hid_keyboard_input_report_boot_t *)data;

    uint8_t modifiers = report->modifier.val;
    uint8_t keycodes[6];
    memcpy(keycodes, report->key, sizeof(keycodes));

    if (filter_keyboard_report(&modifiers, keycodes)) {
        send_keyboard_report(modifiers, keycodes);
    }
}

static void handle_mouse_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }
    const hid_mouse_input_report_boot_t *report = (const hid_mouse_input_report_boot_t *)data;

    uint8_t buttons = report->buttons.val;
    int8_t dx = report->x_displacement;
    int8_t dy = report->y_displacement;

    if (filter_mouse_report(&buttons, &dx, &dy)) {
        send_mouse_report(buttons, dx, dy);
    }
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void)arg;
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t data_length = 0;
            if (hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length) != ESP_OK) {
                return;
            }
            if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                    handle_keyboard_report(data, data_length);
                } else if (dev_params.proto == HID_PROTOCOL_MOUSE) {
                    handle_mouse_report(data, data_length);
                }
            }
            // Non-boot ("generic") HID devices are ignored for now - see
            // mds/2026-08-21_usb_host.md.
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID device disconnected (proto %d)", dev_params.proto);
            hid_host_device_close(hid_device_handle);
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID device transfer error (proto %d)", dev_params.proto);
            break;
        default:
            break;
    }
}

static void handle_driver_connected(hid_host_device_handle_t hid_device_handle)
{
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback     = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(hid_device_handle, &dev_config) != ESP_OK) {
        return;
    }

    if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        // Force Boot Protocol so reports are the fixed, well-known layout
        // (hid_keyboard_input_report_boot_t / hid_mouse_input_report_boot_t)
        // instead of a device-specific Report Protocol we'd have to parse
        // against the HID report descriptor ourselves.
        hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(hid_device_handle, 0, 0);
        }
        ESP_LOGI(TAG, "HID device connected (proto %d)", dev_params.proto);
    } else {
        ESP_LOGI(TAG, "HID device connected (generic, not Boot Protocol - ignored)");
    }

    hid_host_device_start(hid_device_handle);
}

static void hid_host_driver_event_callback(hid_host_device_handle_t hid_device_handle,
                                           const hid_host_driver_event_t event,
                                           void *arg)
{
    (void)arg;
    if (s_driver_event_queue) {
        xQueueSend(s_driver_event_queue, &hid_device_handle, 0);
    }
    (void)event; // Only HID_HOST_DRIVER_EVENT_CONNECTED exists today.
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_host_app_task(void *arg)
{
    (void)arg;

    hid_host_device_handle_t hid_device_handle;
    while (1) {
        if (xQueueReceive(s_driver_event_queue, &hid_device_handle, portMAX_DELAY)) {
            handle_driver_connected(hid_device_handle);
        }
    }
}

esp_err_t usb_host_task_start(void)
{
    if (!resolve_target()) {
        return ESP_FAIL;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        return ESP_FAIL;
    }

    s_driver_event_queue = xQueueCreate(10, sizeof(hid_host_device_handle_t));
    if (!s_driver_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t app_task_handle;
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                xTaskGetCurrentTaskHandle(), 2, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority           = 5,
        .stack_size              = 4096,
        .core_id                 = 0,
        .callback                = hid_host_driver_event_callback,
        .callback_arg            = NULL,
    };
    esp_err_t err = hid_host_install(&hid_host_driver_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(usb_host_app_task, "usb_host_app", 4096,
                                NULL, 5, &app_task_handle, 1) != pdTRUE) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Forwarding USB HID input to %s:%d", KVM_TARGET_HOST, UDP_PORT);
    return ESP_OK;
}
