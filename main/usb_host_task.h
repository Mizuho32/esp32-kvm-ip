#ifndef USB_HOST_TASK_H
#define USB_HOST_TASK_H

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST).
 *
 * Starts the USB Host stack and HID class driver (hub-aware - multiple
 * connected keyboards/mice are all forwarded), and a background task that
 * reads Boot Protocol reports from connected devices, runs them through
 * filter_rules.h, and forwards them to KVM_TARGET_HOST over the same UDP
 * protocol server.py uses (protocol.h). The Device-role esp32-kvm-ip board
 * at KVM_TARGET_HOST needs no changes to receive these packets.
 *
 * Must be called after WiFi is connected (wifi_manager_init()).
 *
 * @return ESP_OK on success.
 */
esp_err_t usb_host_task_start(void);

#endif
