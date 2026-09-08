#include "mruby_webui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_hid_device.h"
#include "mruby_filter.h"
#include "usb_descriptors.h"

#define TAG "MRBWEBUI"

// EMBED_TXTFILES (main/CMakeLists.txt) fallback for the page below - used
// whenever webui_html (below) is empty/erased/invalid, same relationship
// as mruby_filter.c's embedded default.rb / mrb_script partition. All
// dynamic content (current script text, active/hostname/frontend status)
// is fetched by the page's own JS from /api/* after load, rather than
// templated in here - that keeps this file to plain byte-serving only:
// no HTML-escaping of arbitrary script content is ever needed on the C
// side (a script's `#`/`<`/`&` etc. never touch an HTML document - it
// only ever goes into a JS string via textarea/CodeMirror value, which
// browsers handle as opaque text).
extern const uint8_t webui_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t webui_index_html_end[]   asm("_binary_index_html_end");

// Matches bin/upload_mruby_script.py's per-partition size check
// (partitions.csv's mrb_script partition is 64K, minus the 4-byte length
// header mruby_filter.c's mruby_filter_write_script()/load_uploaded_script()
// use).
#define MAX_SCRIPT_SIZE (64 * 1024 - 4)

// Raw (non-filesystem) storage for one uploaded copy of the WebUI's own
// frontend page - same on-flash format (4-byte little-endian length +
// bytes) and same upload tool (bin/upload_mruby_script.py --partition
// webui_html) as mrb_script, see partitions.csv and
// mds/usb_hid/2026-08-30_mruby_phase2_webui.md. Unlike the mruby script,
// this is read fresh on every GET / (no VM/backend state depends on
// it), so uploading a new one takes effect immediately - no reboot.
#define WEBUI_HTML_PARTITION_LABEL   "webui_html"
#define WEBUI_HTML_PARTITION_SUBTYPE 0x51
#define MAX_FRONTEND_SIZE (32 * 1024 - 4)

static httpd_handle_t s_server;

// Prefers PSRAM for this file's buffers (they're only alive for the
// duration of one HTTP request - script/frontend text, at most tens of
// KB) so they stop competing with mruby's VM heap and every task's stack
// for the ~300-400K of internal SRAM alone (this board has no PSRAM
// registered with plain malloc() - see sdkconfig.defaults'
// SPIRAM_USE_CAPS_ALLOC comment - so mruby's own allocations are
// unaffected by this). Falls back to regular (internal) malloc() if
// PSRAM isn't available for any reason (not enabled, physically absent,
// or exhausted) - same behavior this code had before PSRAM was added.
// See mds/usb_hid/2026-08-30_mruby_phase2_webui.md.
static void *webui_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return (p != NULL) ? p : malloc(size);
}

// Mirrors mruby_filter.c's read_script_partition_raw() for the
// webui_html partition - kept separate (not a shared helper) since each
// file owns a different partition and the two have no other overlap.
// Finds webui_html and reads/validates just its 4-byte length header (no
// content read/allocation) - NULL if missing/erased/corrupt. Mirrors
// mruby_filter.c's find_mrb_script_partition().
static const esp_partition_t *find_webui_html_partition(uint32_t *out_len)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, WEBUI_HTML_PARTITION_SUBTYPE, WEBUI_HTML_PARTITION_LABEL);
    if (part == NULL) {
        return NULL;
    }
    uint32_t len;
    if (esp_partition_read(part, 0, &len, sizeof(len)) != ESP_OK) {
        return NULL;
    }
    if (len == 0 || len == 0xFFFFFFFFu || len > part->size - sizeof(len)) {
        return NULL; // never uploaded (erased flash reads as 0xFF) or corrupt
    }
    *out_len = len;
    return part;
}

static bool read_webui_html_partition(char **out_buf, uint32_t *out_len)
{
    uint32_t len;
    const esp_partition_t *part = find_webui_html_partition(&len);
    if (part == NULL) {
        return false;
    }
    char *buf = webui_alloc(len);
    if (buf == NULL) {
        return false;
    }
    if (esp_partition_read(part, sizeof(len), buf, len) != ESP_OK) {
        free(buf);
        return false;
    }
    *out_buf = buf;
    *out_len = len;
    return true;
}

static esp_err_t write_webui_html_partition(const char *data, size_t len)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, WEBUI_HTML_PARTITION_SUBTYPE, WEBUI_HTML_PARTITION_LABEL);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > part->size - sizeof(uint32_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t len_hdr = (uint32_t)len;
    err = esp_partition_write(part, 0, &len_hdr, sizeof(len_hdr));
    if (err != ESP_OK) {
        return err;
    }
    if (len > 0) {
        err = esp_partition_write(part, sizeof(len_hdr), data, len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char *buf;
    uint32_t len;
    if (read_webui_html_partition(&buf, &len)) {
        esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
        free(buf);
        return err;
    }
    return httpd_resp_send(req, (const char *)webui_index_html_start,
                            (ssize_t)(webui_index_html_end - webui_index_html_start));
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    // Sized to the script's actual length, not the worst-case
    // MAX_SCRIPT_SIZE - see mruby_filter_read_script()'s comment
    // (mruby_filter.c) on why this mattered for the "out of memory" 500s
    // this endpoint used to produce intermittently.
    size_t len = mruby_filter_script_len();
    char *buf = webui_alloc(len + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    size_t actual = mruby_filter_read_script(buf, len + 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    // Without this, browsers may serve a cached copy of this response on a
    // plain reload (no explicit Cache-Control/ETag/Last-Modified means
    // heuristic caching is allowed) - the WebUI's own save flow already
    // reboots the board on success, so a reload afterwards is exactly the
    // "did my edit actually take?" check this would silently break.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)actual);
    free(buf);
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    uint32_t tmp_len;
    bool custom_frontend = find_webui_html_partition(&tmp_len) != NULL;

    // "not declared": the loaded script never called `sink :name, :ble`
    // (mruby_filter_ble_sink_declared()), so ble_hid_device_start() never
    // ran at all - no NimBLE stack up, "Unpair" below is a no-op. See
    // mds/usb_hid/2026-09-07_ble_hid_sink_plan.md.
    const char *ble_state = !mruby_filter_ble_sink_declared() ? "not declared by script"
                             : ble_hid_device_connected()      ? "connected"
                                                                : "advertising / not paired";

    char buf[256];
    const char *hostname = mruby_filter_hostname();
    int n = snprintf(buf, sizeof(buf), "mruby: %s\nhostname: %s\nfrontend: %s\nble: %s\n",
                      mruby_filter_active() ? "active" : "inactive (C filter_rules.h/route_rules.h fallback in effect)",
                      hostname ? hostname : "(not set by script)",
                      custom_frontend ? "custom (uploaded via UART)" : "embedded default",
                      ble_state);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store"); // same reasoning as script_get_handler()
    return httpd_resp_send(req, buf, (ssize_t)n);
}

// Manually forces the USB-suspend power-management reaction (WiFi stop,
// see power_manager.c/mds/usb_hid/2026-8-30_Sleep.md) - covers connecting
// to a PC that was already suspended before this board booted, which
// never fires tud_suspend_cb() (no bus transition for tinyusb to notice).
// No separate delay/task needed before responding, unlike restart_task()
// below: usb_device_force_suspended() only sets a flag and gives a task
// notification, it doesn't itself touch WiFi - the actual esp_wifi_stop()
// happens moments later in power_manager_task's own task, well after this
// handler's response has already gone out.
static esp_err_t sleep_post_handler(httpd_req_t *req)
{
    usb_device_force_suspended();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Sleeping - WiFi will stop shortly.\n", HTTPD_RESP_USE_STRLEN);
}

// POST /api/ble_unpair - forgets every bonded BLE central (ble_hid_device_unpair())
// so a different PC can pair next - see ble_hid_device.h's doc comment and
// mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's ペアリング section ("ESP32側
// UIは不要...別PCと再ペアリングしたい用にUnpairボタンを追加"). Safe to call
// even if no `:ble` sink was ever declared by the script (ble_hid_device_unpair()
// is a no-op before ble_hid_device_start() ran) or nothing is currently
// bonded/connected - no separate guard needed here.
static esp_err_t ble_unpair_post_handler(httpd_req_t *req)
{
    ble_hid_device_unpair();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Unpaired - a different PC can pair now.\n", HTTPD_RESP_USE_STRLEN);
}

// Runs esp_restart() from a separate task rather than inline in
// script_post_handler(): that handler's httpd_resp_send() call above it
// only queues the response with the httpd worker task's socket, and
// restarting immediately afterwards risks tearing down the connection
// before the client actually receives it. A short delay on a task of its
// own lets the send (and the worker's own bookkeeping) finish first.
static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Reads req's full body into a malloc'd *out_buf (caller frees; NULL if
// content_len is 0), rejecting anything over max_size. On any failure
// this has already sent the appropriate httpd_resp_send_err() itself -
// callers should just propagate ESP_FAIL. Shared by script_post_handler()
// and frontend_post_handler() below.
static esp_err_t recv_full_body(httpd_req_t *req, char **out_buf, size_t *out_len, size_t max_size)
{
    size_t total = req->content_len;
    if (total > max_size) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "request body too large for its target partition");
        return ESP_FAIL;
    }

    char *buf = NULL;
    if (total > 0) {
        buf = webui_alloc(total);
        if (buf == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_FAIL;
        }
        size_t received = 0;
        while (received < total) {
            int r = httpd_req_recv(req, buf + received, total - received);
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
                return ESP_FAIL;
            }
            received += (size_t)r;
        }
    }

    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

static esp_err_t script_post_handler(httpd_req_t *req)
{
    char *buf;
    size_t total;
    if (recv_full_body(req, &buf, &total, MAX_SCRIPT_SIZE) != ESP_OK) {
        return ESP_FAIL;
    }

    char syntax_err[160];
    if (!mruby_filter_check_syntax(buf, total, syntax_err, sizeof(syntax_err))) {
        ESP_LOGW(TAG, "Rejected script upload (not saved): %s", syntax_err);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, syntax_err);
        return ESP_FAIL;
    }

    esp_err_t err = mruby_filter_write_script(buf, total);
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mruby_filter_write_script failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "New script saved (%d bytes) via WebUI - restarting to apply it", (int)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, "Saved - rebooting...\n", HTTPD_RESP_USE_STRLEN);

    if (xTaskCreate(restart_task, "mrbwebui_restart", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start restart task - reboot manually to apply the new script");
    }
    return ESP_OK;
}

// POST /api/frontend - uploads a replacement for this page itself (see
// webui_html above). Not exposed as a button on the page - only reachable
// via bin/upload_mruby_script.py --partition webui_html over serial, per
// mds/usb_hid/2026-08-30_mruby_phase2_webui.md (a page editing itself
// live over HTTP wasn't asked for and adds failure modes - e.g. a bad
// upload bricking the only way to reach it - that a serial-only path
// avoids). Takes effect immediately, no restart (index_get_handler reads
// the partition fresh on every request).
static esp_err_t frontend_post_handler(httpd_req_t *req)
{
    char *buf;
    size_t total;
    if (recv_full_body(req, &buf, &total, MAX_FRONTEND_SIZE) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = write_webui_html_partition(buf, total);
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write_webui_html_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "New frontend saved (%d bytes) - in effect immediately", (int)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "Saved.\n", HTTPD_RESP_USE_STRLEN);
}

void mruby_webui_start(void)
{
#if !CONFIG_MRUBY_FILTER_ROUTE_ENABLE
    return;
#else
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 16384 (was 8192): script_post_handler() runs on this task, and its
    // mruby_filter_check_syntax() call opens a whole extra mrb_state and
    // runs a full parse+codegen (this mruby's Prism-based
    // mrb_parse_nstring() compiles, not just parses - see that function's
    // comment in mruby_filter.c) - the same class of operation that made
    // the *main* task's own stack need bumping to 16384 twice over
    // (CONFIG_ESP_MAIN_TASK_STACK_SIZE, sdkconfig.defaults) for
    // mruby_filter_init()'s parse+exec of the real script. Confirmed via a
    // real "Guru Meditation Error ... LoadProhibited" panic inside
    // FreeRTOS's own scheduler (prvSelectHighestPriorityTaskSMP, A2 =
    // 0xa5a5a5a5 - FreeRTOS's stack-fill poison byte) right after clicking
    // Save in the WebUI, i.e. a stack overflow here corrupting adjacent
    // memory rather than crashing at the overflow site itself.
    config.stack_size = 16384;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start() failed - WebUI not available (bin/upload_mruby_script.py still works)");
        return;
    }

    static const httpd_uri_t index_uri     = { .uri = "/",             .method = HTTP_GET,  .handler = index_get_handler };
    static const httpd_uri_t script_get    = { .uri = "/api/script",   .method = HTTP_GET,  .handler = script_get_handler };
    static const httpd_uri_t script_post   = { .uri = "/api/script",   .method = HTTP_POST, .handler = script_post_handler };
    static const httpd_uri_t status_uri    = { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler };
    static const httpd_uri_t frontend_post = { .uri = "/api/frontend", .method = HTTP_POST, .handler = frontend_post_handler };
    static const httpd_uri_t sleep_post    = { .uri = "/api/sleep",    .method = HTTP_POST, .handler = sleep_post_handler };
    static const httpd_uri_t ble_unpair_post = { .uri = "/api/ble_unpair", .method = HTTP_POST, .handler = ble_unpair_post_handler };
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &script_get);
    httpd_register_uri_handler(s_server, &script_post);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &frontend_post);
    httpd_register_uri_handler(s_server, &sleep_post);
    httpd_register_uri_handler(s_server, &ble_unpair_post);

    ESP_LOGI(TAG, "WebUI listening on port %d", config.server_port);
#endif
}
