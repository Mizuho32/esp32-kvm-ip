#include "ota_updater.h"

#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

#define TAG "OTAUPD"

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static bool s_active;

esp_err_t ota_updater_begin(size_t image_size)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE; // caller bug - finish()/abort() the previous one first
    }

    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        ESP_LOGE(TAG, "no inactive OTA slot found (bad partition table?)");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size > s_target->size) {
        ESP_LOGE(TAG, "image (%u bytes) too big for %s (%" PRIu32 " bytes)",
                  (unsigned)image_size, s_target->label, s_target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_begin(s_target, image_size, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin() failed: %s", esp_err_to_name(err));
        return err;
    }
    s_active = true;
    ESP_LOGI(TAG, "OTA update started -> %s (%u bytes)", s_target->label, (unsigned)image_size);
    return ESP_OK;
}

esp_err_t ota_updater_write(const void *data, size_t len)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write() failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_updater_finish(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false; // clear regardless of outcome below - either way this attempt is done

    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        // Most likely ESP_ERR_OTA_VALIDATE_FAILED (image header/hash
        // mismatch - a truncated/corrupt upload) - the partition just
        // written is left as-is (garbage), but the boot target is
        // untouched, so this is safe: the board keeps running/booting the
        // still-valid current slot.
        ESP_LOGE(TAG, "esp_ota_end() failed (image invalid/corrupt?): %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition() failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA update written and verified - boot partition set to %s", s_target->label);
    return ESP_OK;
}

void ota_updater_abort(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    esp_ota_abort(s_handle);
    ESP_LOGW(TAG, "OTA update aborted");
}
