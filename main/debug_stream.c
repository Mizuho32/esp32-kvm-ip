#include "debug_stream.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TAG "DBGSTREAM"

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

    ESP_LOGI(TAG, "debug stream started on port %d (GET /stream)", DEBUG_STREAM_PORT);
    return ESP_OK;
}

esp_err_t debug_stream_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_OK; // already stopped
    }
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
