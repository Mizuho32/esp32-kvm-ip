#include "crash_report.h"

#include <inttypes.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h" // IP_EVENT/IP_EVENT_STA_GOT_IP
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "CRASH_REPORT"

// Mirrors ble_pair_slots.c's NVS persistence pattern. Single key "last" -
// a short plain-text summary, not the raw coredump (that lives briefly in
// the `coredump` partition and is erased once read here - see this
// module's doc comment in crash_report.h for why the NVS copy, not the
// partition, is the durable record).
#define NVS_NAMESPACE "crash"
#define NVS_KEY_LAST "last"
#define NVS_KEY_COUNT "count"
#define SUMMARY_MAX 200

// Set true by crash_report_init() only when *this* boot's reset reason
// indicates a crash and a coredump was actually found - false on every
// ordinary boot, which is what keeps crash_report_notify_after_wifi()
// from re-pushing a notification for an already-reported crash on every
// subsequent clean reboot.
static bool s_pending_notify;

// script's `crash_notify_url "https://ntfy.sh/..."` - see
// crash_report_set_notify_url()'s doc comment (crash_report.h).
static char s_notify_url[128];
static bool s_notify_url_set;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
    }
}

// Which reset reasons are worth digging a coredump out for - a plain
// esp_restart() (ESP_RST_SW, e.g. after an OTA update) or power-on/deep-
// sleep-wake are all *expected* reboots with nothing to report. The rest
// are all reboots this project's own code never asked for.
static bool is_crash_like(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP;
}

// Raw read of the previously-saved summary text, without the "[recurred
// Nx]" suffix crash_report_last_text() (public API) adds - crash_report_init()
// below needs the bare text to compare against a freshly-built one.
static bool read_saved_summary(char *out, size_t out_size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(h, NVS_KEY_LAST, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

static uint32_t read_saved_count(void)
{
    nvs_handle_t h;
    uint32_t count = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_COUNT, &count);
        nvs_close(h);
    }
    return count;
}

static void save_summary(const char *text, uint32_t count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open() failed - this crash summary won't survive a reboot");
        return;
    }
    nvs_set_str(h, NVS_KEY_LAST, text);
    nvs_set_u32(h, NVS_KEY_COUNT, count);
    nvs_commit(h);
    nvs_close(h);
}

void crash_report_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (!is_crash_like(reason)) {
        // Ordinary boot - leave whatever's already saved in NVS alone
        // (that's the whole point: it should keep showing up in the
        // WebUI across any number of further clean reboots, not just the
        // one right after the crash).
        return;
    }

    char summary[SUMMARY_MAX];
    int n = snprintf(summary, sizeof summary, "reset=%s", reset_reason_str(reason));

    char panic_reason[120];
    if (esp_core_dump_get_panic_reason(panic_reason, sizeof panic_reason) == ESP_OK) {
        n += snprintf(summary + n, sizeof(summary) - (size_t)n, ": %s", panic_reason);
    }

    esp_core_dump_summary_t core_summary;
    if (esp_core_dump_get_summary(&core_summary) == ESP_OK) {
        n += snprintf(summary + n, sizeof(summary) - (size_t)n, " (task: %s, pc: 0x%08" PRIx32 ")",
                      core_summary.exc_task, core_summary.exc_pc);
    }
    (void)n;

    ESP_LOGE(TAG, "previous boot crashed - %s", summary);

    // Same reset reason/panic reason/task/PC as the one already on file -
    // this is the *same* bug crashing again (most commonly a boot loop:
    // crash -> reboot -> crash again before anyone's had a chance to fix
    // or even see it), not a new one. Bump the repeat count instead of
    // re-notifying every single time - a boot-looping board would
    // otherwise flood ntfy with dozens of identical pushes. The WebUI's
    // "Clear last crash" (crash_report_clear()) is what resets this: it
    // erases both keys below, so the *next* occurrence - even of this
    // exact same still-unfixed bug - is "new" again and notifies once
    // more. That's the "mark this id resolved" mechanism, reusing the
    // dismiss button that already existed for a different reason.
    char prev[SUMMARY_MAX];
    if (read_saved_summary(prev, sizeof prev) && strcmp(prev, summary) == 0) {
        uint32_t count = read_saved_count() + 1;
        save_summary(summary, count);
        ESP_LOGW(TAG, "same crash as last time (seen %" PRIu32 "x since last cleared) - notification suppressed", count);
    } else {
        save_summary(summary, 1);
        s_pending_notify = true;
    }

    // The NVS copy above is now the durable record - free the partition
    // for the next actual crash. Harmless if there was nothing to erase
    // (is_crash_like() being true doesn't guarantee a coredump was
    // actually written, e.g. a brownout right at boot).
    esp_core_dump_image_erase();
}

void crash_report_set_notify_url(const char *url, size_t len)
{
    if (len >= sizeof s_notify_url) {
        len = sizeof(s_notify_url) - 1;
    }
    memcpy(s_notify_url, url, len);
    s_notify_url[len] = '\0';
    s_notify_url_set = len > 0;
}

// Runs in its own short-lived task (not the esp_event task that invoked
// the IP_EVENT_STA_GOT_IP handler below) since esp_http_client_perform()
// blocks - potentially for a couple of seconds with a TLS handshake -
// and holding up the system event loop for that isn't worth the risk of
// delaying every other event handler that shares it.
static void notify_task(void *arg)
{
    (void)arg;
    char summary[SUMMARY_MAX] = "";
    crash_report_last_text(summary, sizeof summary);

    esp_http_client_config_t config = {
        .url = s_notify_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, // https:// (ntfy.sh) needs a CA bundle
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        esp_http_client_set_header(client, "Title", "Wireless USBHID crashed");
        esp_http_client_set_post_field(client, summary, (int)strlen(summary));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "crash notify POST failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
    vTaskDelete(NULL);
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    // One-shot: only ever meant to fire for the crash detected earlier
    // this same boot - unregister immediately so a later WiFi
    // reconnect (unrelated to any crash) doesn't re-trigger this.
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler);
    xTaskCreate(notify_task, "crash_notify", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void crash_report_notify_after_wifi(void)
{
    if (!s_pending_notify) {
        return;
    }
    s_pending_notify = false;
    if (!s_notify_url_set) {
        ESP_LOGI(TAG, "crash detected but no crash_notify_url set - WebUI/NVS record still saved");
        return;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler, NULL);
}

void crash_report_last_text(char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!read_saved_summary(out, out_size)) {
        return;
    }
    uint32_t count = read_saved_count();
    if (count > 1) {
        size_t len = strlen(out);
        snprintf(out + len, out_size - len, " [recurred %" PRIu32 "x since last cleared]", count);
    }
}

void crash_report_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    // Erasing both, not just "last": the next crash - even an exact
    // repeat of this same still-unfixed bug - should be treated as "new"
    // again (re-notify once, restart the repeat count at 1) rather than
    // silently folding into whatever count was left over from before
    // this was cleared. See crash_report_init()'s own comment on why.
    nvs_erase_key(h, NVS_KEY_LAST);
    nvs_erase_key(h, NVS_KEY_COUNT);
    nvs_commit(h);
    nvs_close(h);
}

// ═══════════════════════════════════════════════════════════════════
//  On-demand heap tracing (esp_heap_trace.h) - mruby_filter.c's
//  `heap_trace_start`/`heap_trace_dump` DSL commands. See crash_report.h's
//  doc comment: this is the tool that would have pinpointed
//  esp_hid_gap.c's uuid16 malloc leak (mds/usb_hid/2026-09-13_ble_idle_crash.md)
//  to its exact call site immediately, instead of hours of heap_monitor.c
//  free-byte-counting - kept off by default (CONFIG_HEAP_TRACING_STANDALONE
//  merely makes the API available, sdkconfig.defaults) and only allocates/
//  runs when a script actually asks for it.
// ═══════════════════════════════════════════════════════════════════

// Record buffer lives in PSRAM (this project has 8MB of it, and the
// records themselves are just addresses/sizes - no DMA/internal-RAM
// requirement) so an in-progress trace doesn't itself compete with
// whatever internal-RAM headroom is actually being investigated.
#define HEAP_TRACE_NUM_RECORDS 400
static heap_trace_record_t *s_trace_records;

esp_err_t crash_report_heap_trace_start(void)
{
    if (s_trace_records == NULL) {
        s_trace_records = heap_caps_malloc(HEAP_TRACE_NUM_RECORDS * sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM);
        if (s_trace_records == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = heap_trace_init_standalone(s_trace_records, HEAP_TRACE_NUM_RECORDS);
        if (err != ESP_OK) {
            return err;
        }
    }
    // LEAKS (not ALL): a still-allocated block when heap_trace_dump()
    // below runs is exactly what "leak hunt" means here - freed
    // allocations are dropped from the trace as they happen, so the
    // fixed-size record buffer only has to hold what's still live, not
    // every allocation churned through during the whole traced window.
    return heap_trace_start(HEAP_TRACE_LEAKS);
}

void crash_report_heap_trace_dump(void)
{
    heap_trace_stop();
    heap_trace_dump(); // prints each still-allocated record's size + call-site backtrace via ESP_LOG
}
