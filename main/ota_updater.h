#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <stddef.h>

#include "esp_err.h"

// Host role only. Thin wrapper around ESP-IDF's esp_ota_ops for
// mruby_webui.c's POST /api/firmware - streams the uploaded firmware
// image straight into the currently-inactive OTA slot (partitions.csv's
// ota_0/ota_1) a chunk at a time, so the ~2-3MB image is never buffered
// whole in RAM (unlike script_post_handler()/frontend_post_handler()'s
// recv_full_body(), fine for a few KB but not for this). See
// mds/usb_hid/2026-09-10_wifi_ota.md.
//
// Single in-flight update at a time (matches mruby_webui.c's own
// single-worker-task httpd, so this is never actually contended) - begin()
// fails with ESP_ERR_INVALID_STATE if called again before the previous
// one's finish()/abort().

// Starts a new update: finds the inactive slot
// (esp_ota_get_next_update_partition()) and calls esp_ota_begin(),
// erasing only image_size worth of it (not the whole partition) since the
// caller already knows the size upfront (HTTP Content-Length). Fails with
// ESP_ERR_INVALID_SIZE if image_size doesn't fit the slot.
esp_err_t ota_updater_begin(size_t image_size);

// Writes one chunk of the image (esp_ota_write()). Must be called after
// ota_updater_begin() and before ota_updater_finish()/_abort().
esp_err_t ota_updater_write(const void *data, size_t len);

// Validates the fully-written image (esp_ota_end() - checks the app image
// header and hash) and, only if that passes, switches the boot target to
// it (esp_ota_set_boot_partition()). Does NOT reboot - callers should do
// that themselves once they've sent their HTTP response (see
// mruby_webui.c's restart_task()), same as script_post_handler()'s
// existing pattern for applying an uploaded script.
esp_err_t ota_updater_finish(void);

// Abandons an in-progress update (a partial/failed upload) without
// changing the current boot target. Safe to call even if begin() was
// never called (a no-op then).
void ota_updater_abort(void);

#endif
