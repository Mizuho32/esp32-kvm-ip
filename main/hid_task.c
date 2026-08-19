#include "hid_task.h"
#include "protocol.h"
#include "usb_descriptors.h"

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include <string.h>


#define TAG "HID"

extern QueueHandle_t hid_event_queue;

static void wait_for_hid_ready(uint8_t instance) {
    while (!tud_hid_n_ready(instance)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Boot Protocol mouse reports are 8-bit relative X/Y (see
// hid_mouse_report_t) - clamp the wider UDP delta into that range instead
// of silently truncating it. Only used while in Boot mode (BIOS); once the
// OS takes over it switches to Report mode and gets the full 16-bit range.
static int8_t clamp_to_i8(int16_t v) {
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int8_t)v;
}

void hid_task(void *pvParameters) {
    (void)pvParameters;

    hid_event_t event;

    while (1) {
        if (xQueueReceive(hid_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case EVENT_TYPE_MOUSE: {
                    wait_for_hid_ready(ITF_NUM_MOUSE);
                    if (tud_hid_n_get_protocol(ITF_NUM_MOUSE) == HID_PROTOCOL_BOOT) {
                        // BIOS/bootloader: fixed compact Boot Protocol format.
                        hid_mouse_report_t report = {
                            .buttons = event.mouse.buttons,
                            .x       = clamp_to_i8(event.mouse.dx),
                            .y       = clamp_to_i8(event.mouse.dy),
                            .wheel   = event.mouse.wheel,
                            .pan     = event.mouse.pan,
                        };
                        tud_hid_n_report(ITF_NUM_MOUSE, 0, &report, sizeof(report));
                    } else {
                        // OS loaded: full-precision Report Protocol format.
                        mouse_report_t report = {
                            .buttons = event.mouse.buttons,
                            .x       = event.mouse.dx,
                            .y       = event.mouse.dy,
                            .wheel   = event.mouse.wheel,
                            .pan     = event.mouse.pan,
                        };
                        tud_hid_n_report(ITF_NUM_MOUSE, 0, &report, sizeof(report));
                    }
                    break;
                }

                case EVENT_TYPE_KEYBOARD: {
                    // Already the Boot Protocol layout - identical in both
                    // modes, no branching needed.
                    hid_keyboard_report_t report = {
                        .modifier = event.keyboard.modifiers,
                        .reserved = 0x00,
                    };
                    memcpy(report.keycode, event.keyboard.keycodes, 6);
                    wait_for_hid_ready(ITF_NUM_KEYBOARD);
                    tud_hid_n_report(ITF_NUM_KEYBOARD, 0, &report, sizeof(report));
                    break;
                }

                case EVENT_TYPE_CONSUMER: {
                    consumer_report_t report = {
                        .usage_id = event.consumer.usage_id,
                    };
                    wait_for_hid_ready(ITF_NUM_CONSUMER);
                    tud_hid_n_report(ITF_NUM_CONSUMER, 0, &report, sizeof(report));
                    break;
                }

                default:
                    break;
            }
        }
    }
}
