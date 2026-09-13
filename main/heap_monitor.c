#include "heap_monitor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "HEAP_MON";

// 30 minutes: fine-grained enough to catch a leak's shape over a several-
// hour idle stretch without flooding the log - see heap_monitor.h.
#define HEAP_MONITOR_INTERVAL_US (30ULL * 60 * 1000 * 1000)

static esp_timer_handle_t s_timer;

// Same cap set as ble_hid_device.c's own start()/stop() heap logs (and
// deliberately not PSRAM) - NimBLE's fixed-size mempools (the thing that
// failed with BLE_HS_ENOMEM in the crash this was added for) come out of
// internal RAM, not SPIRAM, so that's the pool actually worth watching.
#define HEAP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static void log_heap(void)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, HEAP_CAPS);
    // allocated_blocks climbing step-by-step while free/largest shrink in
    // lockstep would point at a genuine one-object-at-a-time leak (like
    // esp_hid_gap.c's now-fixed uuid16 malloc); free/largest moving with
    // allocated_blocks roughly flat instead would point at fragmentation
    // or a leak that reuses/grows existing blocks rather than adding new
    // ones. minimum_free_bytes is the lifetime low-water mark since boot -
    // it catches a transient dip between two 30-minute samples that the
    // instantaneous numbers alone would miss.
    ESP_LOGI(TAG, "internal: free=%u largest_block=%u min_ever_free=%u "
             "allocated_blocks=%u total_allocated=%u",
             (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block,
             (unsigned)info.minimum_free_bytes, (unsigned)info.allocated_blocks,
             (unsigned)info.total_allocated_bytes);
}

static void timer_cb(void *arg)
{
    (void)arg;
    log_heap();
}

void heap_monitor_init(void)
{
    log_heap(); // baseline, before the first interval has even elapsed

    const esp_timer_create_args_t args = {
        .callback = timer_cb,
        .name = "heap_monitor",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_create failed: %s (periodic heap logging disabled)",
                 esp_err_to_name(err));
        return;
    }
    esp_timer_start_periodic(s_timer, HEAP_MONITOR_INTERVAL_US);
}
