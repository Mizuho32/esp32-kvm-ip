#include "crash_report.h"

#include <inttypes.h>
#include <stdlib.h>
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

// script's `crash_notify_url "https://ntfy.sh/...", "tk_..."` - see
// crash_report_set_notify_url()'s doc comment (crash_report.h). Token
// is optional/empty for a public (unauthenticated) topic - ntfy.sh's
// default - and required for a self-hosted server with access control
// or an ntfy.sh *reserved* topic.
static char s_notify_url[128];
static char s_notify_token[128];
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

    // Every crash notifies, including a repeat of the exact same bug
    // (a real crash is a real crash - a board stuck boot-looping on one
    // unfixed bug is exactly the situation worth *more* pushes, not
    // fewer). The repeat count tracked here is purely informational -
    // "[recurred Nx since last cleared]" in the WebUI (crash_report_last_text())
    // - so a crash loop shows up as "this keeps happening" rather than
    // silently losing count between one notification and the next.
    // crash_report_clear() (the WebUI's dismiss button) resets it back
    // to 1 for whatever crash comes next.
    char prev[SUMMARY_MAX];
    uint32_t count = (read_saved_summary(prev, sizeof prev) && strcmp(prev, summary) == 0)
                          ? read_saved_count() + 1
                          : 1;
    save_summary(summary, count);
    s_pending_notify = true;

    // Deliberately *not* erasing the coredump partition here anymore:
    // ESP-IDF overwrites an existing core dump with a new one by default
    // (CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE, which this project leaves
    // off) - the next crash gets a clean write regardless, no manual
    // erase needed. Leaving the raw ELF dump in place means the actual
    // full backtrace/registers/per-task stacks are still there for a
    // deeper offline look (idf.py coredump-info over serial) if this
    // short summary alone isn't enough to diagnose something new -
    // there's no cost to keeping it (fixed 128K partition either way).
}

static void copy_truncated(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (src_len >= dst_size) {
        src_len = dst_size - 1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

void crash_report_set_notify_url(const char *url, size_t url_len, const char *token, size_t token_len)
{
    copy_truncated(s_notify_url, sizeof s_notify_url, url, url_len);
    copy_truncated(s_notify_token, sizeof s_notify_token, token != NULL ? token : "", token_len);
    s_notify_url_set = s_notify_url[0] != '\0';
}

// Shared by both the real post-crash push (got_ip_handler() below) and
// crash_report_notify_test()'s on-demand one - only the title/body
// differ. Heap-allocated (not a static buffer) since a second call could
// otherwise race a still-running notify_task() from the first; freed by
// notify_task() itself once it's done with it.
typedef struct {
    char title[64];
    char body[SUMMARY_MAX];
} notify_payload_t;

// Runs in its own short-lived task (not the esp_event task that invoked
// the IP_EVENT_STA_GOT_IP handler below, nor whatever mruby DSL call
// triggered crash_report_notify_test()) since esp_http_client_perform()
// blocks - potentially for a couple of seconds with a TLS handshake -
// and holding up either one for that isn't worth the risk.
static void notify_task(void *arg)
{
    notify_payload_t *payload = (notify_payload_t *)arg;

    esp_http_client_config_t config = {
        .url = s_notify_url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, // https:// (ntfy.sh) needs a CA bundle
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        esp_http_client_set_header(client, "Title", payload->title);
        if (s_notify_token[0] != '\0') {
            // ntfy access token (self-hosted server with access control,
            // or an ntfy.sh *reserved* topic) - "tk_..." tokens go in the
            // Authorization header, same as ntfy's own documented `curl
            // -H "Authorization: Bearer tk_..."` usage. Left off entirely
            // (no empty header) for a public/unauthenticated topic -
            // ntfy.sh's default.
            char auth[8 + sizeof s_notify_token];
            snprintf(auth, sizeof auth, "Bearer %s", s_notify_token);
            esp_http_client_set_header(client, "Authorization", auth);
        }
        esp_http_client_set_post_field(client, payload->body, (int)strlen(payload->body));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "crash notify POST failed: %s", esp_err_to_name(err));
        } else {
            // esp_http_client_perform() only reflects transport-level
            // failure - a wrong/missing token still "succeeds" here with
            // an HTTP 401/403 the caller would otherwise never see.
            int status = esp_http_client_get_status_code(client);
            if (status < 200 || status >= 300) {
                ESP_LOGW(TAG, "crash notify POST rejected: HTTP %d (check crash_notify_url's token?)", status);
            } else {
                ESP_LOGI(TAG, "crash notify POST sent");
            }
        }
        esp_http_client_cleanup(client);
    }
    free(payload);
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

    notify_payload_t *payload = malloc(sizeof(notify_payload_t));
    if (payload == NULL) {
        return;
    }
    snprintf(payload->title, sizeof payload->title, "Wireless USBHID crashed");
    crash_report_last_text(payload->body, sizeof payload->body);
    xTaskCreate(notify_task, "crash_notify", 4096, payload, tskIDLE_PRIORITY + 1, NULL);
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

esp_err_t crash_report_notify_test(void)
{
    if (!s_notify_url_set) {
        return ESP_ERR_INVALID_STATE;
    }
    // Unlike got_ip_handler() above, this doesn't wait for
    // IP_EVENT_STA_GOT_IP - `crash_notify_test` is meant to be typed/run
    // interactively (or from a script already up and running), i.e.
    // well after WiFi has long since connected. If it hasn't,
    // esp_http_client_perform() below just fails with a network error
    // like any other request would.
    notify_payload_t *payload = malloc(sizeof(notify_payload_t));
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(payload->title, sizeof payload->title, "Wireless USBHID: test notification");
    snprintf(payload->body, sizeof payload->body,
             "This is a test push from crash_notify_test - crash_notify_url/token is working.");
    xTaskCreate(notify_task, "crash_notify_test", 4096, payload, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
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
    // Erasing both, not just "last": every crash notifies regardless
    // (crash_report_init()'s own comment), so this is purely about the
    // WebUI's repeat display - the next crash, even an exact repeat of
    // this same bug, starts back at count 1 instead of silently
    // continuing whatever count was left over from before this was
    // cleared.
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
