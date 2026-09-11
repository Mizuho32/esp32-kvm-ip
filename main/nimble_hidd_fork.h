#pragma once

// See nimble_hidd_fork.c's file header comment for what this is and why it
// exists (a fork of ESP-IDF's esp_hid component's src/nimble_hidd.c, with
// one behavioral change to nimble_hid_stop_gatts()).

#include "esp_err.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Drop-in replacement for calling the generic
// esp_hidd_dev_init(config, ESP_HID_TRANSPORT_BLE, callback, dev_out) -
// same result (a fully-populated *dev_out, ESP_OK on success), just backed
// by this file's own copy of the BLE HID device profile instead of
// esp_hid's original.
esp_err_t kvm_ble_hidd_dev_init(const esp_hid_device_config_t *config, esp_event_handler_t callback, esp_hidd_dev_t **dev_out);

#ifdef __cplusplus
}
#endif
