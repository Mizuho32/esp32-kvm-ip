#include "mruby_webui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mruby_filter.h"

#define TAG "MRBWEBUI"

// EMBED_TXTFILES (main/CMakeLists.txt) for webui/index.html. The page is
// served completely static - all dynamic content (current script text,
// active/hostname status) is fetched by the page's own JS from /api/*
// after load, rather than templated in here. That keeps this file to
// plain byte-serving only: no HTML-escaping of arbitrary script content
// is ever needed on the C side (a script's `#`/`<`/`&` etc. never touch
// an HTML document - it only ever goes into a JS string via
// textarea.value, which browsers handle as opaque text).
extern const uint8_t webui_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t webui_index_html_end[]   asm("_binary_index_html_end");

// Matches bin/upload_mruby_script.py's MAX_SCRIPT_SIZE (partitions.csv's
// mrb_script partition is 64K, minus the 4-byte length header
// mruby_filter.c's mruby_filter_write_script()/load_uploaded_script()
// use).
#define MAX_SCRIPT_SIZE (64 * 1024 - 4)

static httpd_handle_t s_server;

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)webui_index_html_start,
                            (ssize_t)(webui_index_html_end - webui_index_html_start));
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    char *buf = malloc(MAX_SCRIPT_SIZE + 1);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    size_t len = mruby_filter_read_script(buf, MAX_SCRIPT_SIZE + 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[128];
    const char *hostname = mruby_filter_hostname();
    int n = snprintf(buf, sizeof(buf), "mruby: %s\nhostname: %s\n",
                      mruby_filter_active() ? "active" : "inactive (C filter_rules.h/route_rules.h fallback in effect)",
                      hostname ? hostname : "(not set by script)");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, buf, (ssize_t)n);
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

static esp_err_t script_post_handler(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total > MAX_SCRIPT_SIZE) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                             "script too large (mrb_script partition holds at most 64K-4 bytes)");
        return ESP_FAIL;
    }

    char *buf = NULL;
    if (total > 0) {
        buf = malloc(total);
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

void mruby_webui_start(void)
{
#if !CONFIG_MRUBY_FILTER_ROUTE_ENABLE
    return;
#else
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // same rationale as every other mruby-adjacent task - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start() failed - WebUI not available (bin/upload_mruby_script.py still works)");
        return;
    }

    static const httpd_uri_t index_uri  = { .uri = "/",            .method = HTTP_GET,  .handler = index_get_handler };
    static const httpd_uri_t script_get = { .uri = "/api/script",  .method = HTTP_GET,  .handler = script_get_handler };
    static const httpd_uri_t script_post = { .uri = "/api/script", .method = HTTP_POST, .handler = script_post_handler };
    static const httpd_uri_t status_uri = { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get_handler };
    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &script_get);
    httpd_register_uri_handler(s_server, &script_post);
    httpd_register_uri_handler(s_server, &status_uri);

    ESP_LOGI(TAG, "WebUI listening on port %d", config.server_port);
#endif
}
