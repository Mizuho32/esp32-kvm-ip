#include "debug_stream.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TAG "DBGSTREAM"

// Persists only the user's on/off *intent* across a board reboot (mruby
// script Save, firmware update, power cycle, ...) - NOT re-derived from
// debug_stream_active() itself, which always starts false again on
// boot (this instance is never auto-started otherwise, see the header's
// "Deliberately NOT started at boot" comment). Mirrors wifi_manager.c's
// own nvs_open()/nvs_get_*()/nvs_set_*()/nvs_commit() pattern (NVS_NAMESPACE
// there). Namespace/key names are short: NVS caps both at 15 bytes.
#define NVS_NAMESPACE  "dbgstream"
#define NVS_KEY_ENABLED "on"

// ESP-IDF's httpd_start() binds a loopback UDP "control socket" per
// instance at config.ctrl_port (used internally by httpd_stop() to signal
// the server task to shut down - see components/esp_http_server/src/httpd_main.c).
// HTTPD_DEFAULT_CONFIG() always uses the same default (ESP_HTTPD_DEF_CTRL_PORT,
// 32768) - mruby_webui.c's always-running main WebUI instance already
// occupies that port, so this instance needs an explicit, different one
// or httpd_start() below fails to bind it.
#define DEBUG_STREAM_CTRL_PORT 32769

// One viewer expected (see mruby_webui.c's WebUI - explicit Start/Stop,
// one browser tab at a time in practice) - +1 headroom so a stale
// half-closed connection doesn't block a fresh reconnect; lru_purge_enable
// below lets the newest connection evict the oldest if both slots are full.
#define DEBUG_STREAM_MAX_SOCKETS 2

// Just relays short lines in a loop - no mruby/parsing work happens on
// this task, unlike mruby_webui.c's main instance (16384, see its own
// comment) - keep this small.
#define DEBUG_STREAM_STACK_SIZE 4096

#define DEBUG_STREAM_LINE_MAX  120
#define DEBUG_STREAM_QUEUE_LEN 16

// How often the /stream handler's queue wait times out to re-check
// s_should_stop - see debug_stream_stop()'s doc comment in the header for
// why this can't just be portMAX_DELAY.
#define DEBUG_STREAM_POLL_MS 200

typedef struct {
    char text[DEBUG_STREAM_LINE_MAX];
} stream_line_t;

static httpd_handle_t s_httpd;
static QueueHandle_t s_queue;
static volatile bool s_should_stop;

// Reads the persisted on/off intent, defaulting to false (never started)
// if the key was never written (fresh NVS, or an old firmware image that
// predates this) or the read otherwise fails.
static bool load_persisted_enabled(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_ENABLED, &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

static void save_persisted_enabled(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open() failed - on/off state won't survive a reboot this time");
        return;
    }
    nvs_set_u8(h, NVS_KEY_ENABLED, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void debug_stream_init(void)
{
    s_httpd = NULL;
    s_should_stop = false;
    // Created once, kept for the whole process lifetime (not torn down in
    // debug_stream_stop()) - see this file's header comment. Deliberately
    // simpler/safer than creating and destroying it alongside the httpd
    // instance: debug_stream_push() below reads s_queue from whatever
    // task called mruby's debug_print (mruby_filter.c's s_mrb_mutex held,
    // but that has nothing to do with *this* module's own state), fully
    // independent of when debug_stream_start()/_stop() run on a totally
    // different task - keeping the queue itself immortal turns that into
    // a non-issue (worst case a push lands in a queue nobody's currently
    // draining, which just sits there or gets silently overwritten later -
    // never a use-after-free).
    if (s_queue == NULL) {
        s_queue = xQueueCreate(DEBUG_STREAM_QUEUE_LEN, sizeof(stream_line_t));
    }

    // Resume automatically if it was left running before the last reboot
    // (mruby script Save, firmware update, ...) rather than defaulting
    // back to stopped every time - the WebUI's Save/Update-firmware flows
    // already reboot the board on every edit, so "stopped until you
    // explicitly click Stop again" would mean re-clicking Start after
    // nearly every save while debugging. Called from mruby_webui_start()
    // (main_host.c), i.e. after wifi_manager_start() has brought up the
    // TCP/IP thread - safe to open a listening socket here already, same
    // as the main WebUI instance right beside it.
    if (load_persisted_enabled()) {
        ESP_LOGI(TAG, "debug stream was left on before reboot - resuming");
        debug_stream_start();
    }
}

bool debug_stream_active(void)
{
    return s_httpd != NULL;
}

void debug_stream_push(const char *line)
{
    if (s_httpd == NULL) {
        return; // not started - nobody's listening, don't bother queuing
    }
    stream_line_t item;
    strncpy(item.text, line, sizeof(item.text) - 1);
    item.text[sizeof(item.text) - 1] = '\0';
    // Non-blocking (0 tick wait): called from mruby's hot dispatch path
    // (mruby_filter.c's dsl_debug_print(), s_mrb_mutex held) - if the
    // queue is full (the /stream consumer stalled, or nobody's actually
    // connected despite the instance being up), just drop this line
    // rather than ever wait.
    xQueueSend(s_queue, &item, 0);
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // Separate port = separate origin from the browser's point of view
    // (WebUI's own page is served from port 80) - EventSource/fetch enforce
    // CORS on cross-origin reads, so without this header the connection
    // succeeds at the network level but the page's JS silently never sees
    // any data.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    stream_line_t item;
    char frame[DEBUG_STREAM_LINE_MAX + 16];
    while (!s_should_stop) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(DEBUG_STREAM_POLL_MS)) != pdTRUE) {
            continue; // timeout - just loop back around and re-check s_should_stop
        }
        int n = snprintf(frame, sizeof(frame), "data: %s\n\n", item.text);
        if (httpd_resp_send_chunk(req, frame, n) != ESP_OK) {
            return ESP_FAIL; // client gone - let esp_http_server clean up the session
        }
    }
    httpd_resp_send_chunk(req, NULL, 0); // deliberate stop (debug_stream_stop()) - end cleanly
    return ESP_OK;
}

esp_err_t debug_stream_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK; // already running - idempotent (e.g. a second tab clicking Start)
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE; // debug_stream_init() was never called
    }
    s_should_stop = false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DEBUG_STREAM_PORT;
    config.ctrl_port = DEBUG_STREAM_CTRL_PORT;
    config.max_open_sockets = DEBUG_STREAM_MAX_SOCKETS;
    config.stack_size = DEBUG_STREAM_STACK_SIZE;
    config.max_uri_handlers = 1;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start() failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    static const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
    };
    httpd_register_uri_handler(s_httpd, &stream_uri);

    save_persisted_enabled(true); // remember across the next reboot too
    ESP_LOGI(TAG, "debug stream started on port %d (GET /stream)", DEBUG_STREAM_PORT);
    return ESP_OK;
}

esp_err_t debug_stream_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_OK; // already stopped
    }
    save_persisted_enabled(false); // an explicit Stop is what should stick, not auto-resume next boot
    // Tell the /stream handler (if a client is currently connected, its
    // loop is running on *this instance's own task*) to return next time
    // its bounded xQueueReceive() wait times out. This has to happen
    // *before* httpd_stop() below: httpd_stop() sends its shutdown signal
    // over a loopback control socket that the instance's task only
    // notices from its own select() loop (components/esp_http_server/src/httpd_main.c's
    // httpd_server()) - which it can't reach while still inside our
    // handler's call. httpd_stop() itself then blocks *its caller*
    // (polling every 100ms) until that task actually exits. Skipping this
    // flag (or giving the handler an unbounded wait instead of
    // DEBUG_STREAM_POLL_MS) would mean a connected client makes
    // httpd_stop() hang forever - and since this is called from
    // mruby_webui.c's own httpd worker task (POST /api/debug_stream/stop),
    // that would freeze the *main* WebUI too, not just this instance.
    // Confirmed against esp_http_server's own source, not just assumed.
    s_should_stop = true;
    esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "debug stream stopped");
    return err;
}

// ── Boot-time backlog ───────────────────────────────────────────────────
// See debug_stream.h's doc comment on this section for the motivation
// (mruby_filter_init()'s debug_print() calls run before this file's httpd
// instance - or even its queue - can possibly exist).
//
// PSRAM (MALLOC_CAP_SPIRAM), not internal SRAM: this data has no latency
// requirement (rendered into an HTTP response once per WebUI page load,
// not touched on any hot path), and internal SRAM is the genuinely scarce
// resource here once BLE is resident (mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md
// measured its largest free block dropping from 61440 to 17408 bytes from
// that alone) - mirrors mruby_alloc_psram.c's own reasoning for moving
// mruby's heap off internal SRAM entirely. Confirmed safe to allocate
// this early: the boot log shows esp_psram's pool already added to the
// heap allocator (`esp_psram: Adding pool of 8192K of PSRAM memory...`)
// well before app_main() itself starts, let alone mruby_filter_init().
static char (*s_backlog)[DEBUG_BACKLOG_LINE_MAX]; // lazily heap_caps_malloc()'d
static int s_backlog_next;   // ring buffer write cursor
static int s_backlog_count;  // how many valid lines so far (<= DEBUG_BACKLOG_LINES)

void debug_stream_record_recent(const char *line)
{
    if (s_backlog == NULL) {
        s_backlog = heap_caps_malloc(DEBUG_BACKLOG_LINES * sizeof(*s_backlog), MALLOC_CAP_SPIRAM);
        if (s_backlog == NULL) {
            return; // PSRAM alloc failed - just skip the backlog, not fatal
        }
    }
    strncpy(s_backlog[s_backlog_next], line, DEBUG_BACKLOG_LINE_MAX - 1);
    s_backlog[s_backlog_next][DEBUG_BACKLOG_LINE_MAX - 1] = '\0';
    s_backlog_next = (s_backlog_next + 1) % DEBUG_BACKLOG_LINES;
    if (s_backlog_count < DEBUG_BACKLOG_LINES) {
        s_backlog_count++;
    }
}

size_t debug_stream_recent_backlog(char *buf, size_t buf_size)
{
    if (buf_size == 0) {
        return 0;
    }
    if (s_backlog == NULL || s_backlog_count == 0) {
        buf[0] = '\0';
        return 0;
    }
    // Oldest first: once the ring has wrapped (count == DEBUG_BACKLOG_LINES),
    // the oldest line is the one right after the next write slot;
    // otherwise nothing has wrapped yet and the oldest is simply index 0.
    int start = (s_backlog_count < DEBUG_BACKLOG_LINES) ? 0 : s_backlog_next;
    size_t off = 0;
    for (int i = 0; i < s_backlog_count; i++) {
        int idx = (start + i) % DEBUG_BACKLOG_LINES;
        int n = snprintf(buf + off, buf_size - off, "%s\n", s_backlog[idx]);
        if (n < 0 || (size_t)n >= buf_size - off) {
            break; // wouldn't fit - stop rather than truncate mid-line
        }
        off += (size_t)n;
    }
    return off;
}
