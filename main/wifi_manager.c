#include "wifi_manager.h"

#include <string.h>
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"

#include "status_led.h"

#define TAG "WIFI"

// See wifi_manager_load_credentials() below. Custom subtype 0x52 is in
// ESP-IDF's user-defined data-subtype range (0x40-0xFE), same range as
// mruby_filter.c's mrb_script (0x50) / mruby_webui.c's webui_html (0x51) -
// see partitions.csv.
#define WIFI_CRED_PARTITION_LABEL   "wifi_cred"
#define WIFI_CRED_PARTITION_SUBTYPE 0x52
#define WIFI_CRED_MAX_LEN           256 // matches partitions.csv's wifi_cred size

#define WIFI_INIT_DONE_BIT   BIT2
#define NVS_NAMESPACE        "wifi_cache"
#define FAST_CONNECT_TIMEOUT_MS  10000

// Some old/cheap 802.11n APs have interop bugs that make the ESP32 fail
// basic 802.11 authentication (reason 2/205, before WPA is even involved).
// After this many consecutive failures, drop to 802.11b/g-only, which
// sidesteps the AP's 11n code path entirely.
#define PROTOCOL_FALLBACK_RETRY_COUNT 5

// Fallback used when nothing overrides it below - kept in sync by hand
// with mruby_filter.c's own s_wifi_reconnect_restart_after default (no
// shared header constant, same as usb_suspend_wifi_sleep's two
// independently-defaulting "true"s - see wifi_reconnect_restart_after()).
#define DEFAULT_WIFI_RECONNECT_RESTART_AFTER 20

// Host role's mruby_filter.c (KVM_ROLE=HOST only) may define this to let a
// script tune how many *consecutive* reconnect failures (this survives
// across the reason-201 full-scan fallback and the 802.11b/g protocol
// downgrade above - both are just different reconnect attempts, still
// counted) to tolerate before giving up and rebooting outright. Weak/
// bodyless for the same reason as power_manager.c's mruby lookup:
// KVM_ROLE=DEVICE builds don't compile mruby_filter.c at all, so this
// resolves to NULL there and wifi_reconnect_restart_after() falls back to
// the hardcoded default.
extern int mruby_filter_wifi_reconnect_restart_after(void) __attribute__((weak));

static int wifi_reconnect_restart_after(void)
{
    if (mruby_filter_wifi_reconnect_restart_after) {
        return mruby_filter_wifi_reconnect_restart_after();
    }
    return DEFAULT_WIFI_RECONNECT_RESTART_AFTER;
}

EventGroupHandle_t wifi_event_group;
static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;
static bool s_fast_connect = false;
static bool s_protocol_downgraded = false;
// Set on the first-ever IP_EVENT_STA_GOT_IP since boot, never cleared -
// distinguishes "still trying to connect for the first time" (LED
// blinks, status_led_set_blinking(true) in wifi_manager_start()) from a
// later drop-and-reconnect once we know the network is reachable (LED
// just goes off while retrying, no blink - see event_handler()'s
// WIFI_EVENT_STA_DISCONNECTED branch).
static bool s_ever_connected = false;
// Set by wifi_manager_suspend(), cleared by wifi_manager_resume() - tells
// event_handler's WIFI_EVENT_STA_DISCONNECTED branch that esp_wifi_stop()
// itself is the cause, not a real drop, so it should skip the
// retry-forever logic (there's nothing to reconnect to yet).
static bool s_suspending = false;
// Set once by wifi_manager_start(), read later by
// wifi_manager_wait_connected() (via wifi_fallback_connect()) - promoted
// from a wifi_manager_init() local to file scope so it survives the
// split between the two.
static wifi_config_t s_wifi_config;

// ── NVS helpers ──────────────────────────────────────────────────

typedef struct {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint32_t ip;
    uint32_t gw;
    uint32_t netmask;
} wifi_cache_t;

static esp_err_t nvs_load_cache(wifi_cache_t *cache)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(*cache);
    err = nvs_get_blob(h, "cache", cache, &len);
    nvs_close(h);
    return err;
}

static void nvs_save_cache(const wifi_cache_t *cache)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cache", cache, sizeof(*cache));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved WiFi cache (ch=%d)", cache->channel);
}

static void nvs_clear_cache(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "WiFi cache cleared");
}

// ── Save current connection params to NVS ────────────────────────

static void save_current_connection(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) return;

    wifi_cache_t cache = {
        .channel = ap.primary,
        .ip      = ip_info.ip.addr,
        .gw      = ip_info.gw.addr,
        .netmask = ip_info.netmask.addr,
    };
    memcpy(cache.bssid, ap.bssid, 6);
    nvs_save_cache(&cache);
}

// ── Restore DHCP ─────────────────────────────────────────────────

static void restore_dhcp(void)
{
    esp_netif_ip_info_t zero = { 0 };
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_set_ip_info(s_sta_netif, &zero);
    esp_netif_dhcpc_start(s_sta_netif);
}

// ── Drop the cached BSSID/channel pin, back to a normal full scan ──

// Used both by wifi_fallback_connect() (fast-reconnect never got an IP at
// boot) and event_handler() (WIFI_REASON_NO_AP_FOUND after a previously
// successful connection - see there). Only clears config/cache/IP mode;
// callers are responsible for calling esp_wifi_connect() themselves
// afterwards.
static void wifi_reset_to_full_scan(void)
{
    ESP_LOGW(TAG, "Clearing cached BSSID/channel, falling back to full scan");
    nvs_clear_cache();
    restore_dhcp();

    s_wifi_config.sta.bssid_set = false;
    s_wifi_config.sta.channel = 0;
    memset(s_wifi_config.sta.bssid, 0, 6);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config() failed: %s", esp_err_to_name(err));
    }
}

// ── Event handler ────────────────────────────────────────────────

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_DISCONNECTED_BIT);
        if (s_suspending) {
            // wifi_manager_suspend()'s esp_wifi_stop() causes this same
            // event - nothing to reconnect to yet, so skip the
            // retry-forever logic below entirely.
            return;
        }
        // Only matters once we've connected before - before that, the
        // blink status_led_set_blinking(true) started in
        // wifi_manager_start() just keeps running unattended through any
        // number of retries below, no extra handling needed here.
        if (s_ever_connected) {
            status_led_set(false);
        }

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_retry_num++;
        int delay_ms = (s_retry_num < 10) ? (s_retry_num * 1000) : 10000;
        ESP_LOGW(TAG, "Disconnected (reason %d). Reconnecting in %d ms (attempt %d)...",
                 disc->reason, delay_ms, s_retry_num);

        if (disc->reason == WIFI_REASON_NO_AP_FOUND && s_wifi_config.sta.bssid_set) {
            // The fast-reconnect cached BSSID/channel no longer matches
            // any AP in range (router likely changed channel or
            // restarted) - unlike wifi_manager_wait_connected()'s own
            // fallback, this auto-retry path would otherwise keep
            // retrying the exact same stale BSSID/channel forever,
            // failing with this same reason every time (this is what
            // produced the reason-201 lockup seen during testing).
            wifi_reset_to_full_scan();
        } else if (!s_protocol_downgraded && s_retry_num >= PROTOCOL_FALLBACK_RETRY_COUNT) {
            s_protocol_downgraded = true;
            ESP_LOGW(TAG, "Repeated connection failures, falling back to 802.11b/g only");
            esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set 802.11b/g-only protocol: %s", esp_err_to_name(err));
            }
        }

        int restart_after = wifi_reconnect_restart_after();
        if (restart_after > 0 && s_retry_num >= restart_after) {
            ESP_LOGE(TAG, "Failed to reconnect after %d attempts - restarting", s_retry_num);
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        status_led_set(true); // also stops the connecting-blink, if it was still running
        s_ever_connected = true;
        s_retry_num = 0;
        xEventGroupClearBits(wifi_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_INIT_DONE_BIT);
        save_current_connection();
    }
}

// ── Apply static IP from cache ───────────────────────────────────

static void apply_static_ip(const wifi_cache_t *cache)
{
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t ip_info = {
        .ip.addr      = cache->ip,
        .gw.addr      = cache->gw,
        .netmask.addr = cache->netmask,
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ip_info));
    ESP_LOGI(TAG, "Static IP set: " IPSTR, IP2STR(&ip_info.ip));
}

// ── Fallback Full Scan ───────────────────────────────────────────

static esp_err_t wifi_fallback_connect(void)
{
    ESP_LOGW(TAG, "Fast reconnect failed, falling back to full scan");
    esp_wifi_disconnect();
    wifi_reset_to_full_scan();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_INIT_DONE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_INIT_DONE_BIT) {
        ESP_LOGI(TAG, "WiFi connected (fallback)");
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ── Public API ───────────────────────────────────────────────────

bool wifi_manager_load_credentials(char *ssid_out, size_t ssid_cap,
                                    char *password_out, size_t password_cap,
                                    char *hostname_out, size_t hostname_cap)
{
    if (ssid_cap > 0) {
        ssid_out[0] = '\0';
    }
    if (password_cap > 0) {
        password_out[0] = '\0';
    }
    if (hostname_out != NULL && hostname_cap > 0) {
        hostname_out[0] = '\0';
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, WIFI_CRED_PARTITION_SUBTYPE, WIFI_CRED_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGW(TAG, "wifi_cred partition not found");
        return false;
    }

    uint32_t len;
    if (esp_partition_read(part, 0, &len, sizeof(len)) != ESP_OK) {
        return false;
    }
    if (len == 0 || len == 0xFFFFFFFFu || len > part->size - sizeof(len)) {
        ESP_LOGW(TAG, "wifi_cred not uploaded yet (erased/empty) - use bin/upload_wifi_credentials.py");
        return false;
    }

    char buf[WIFI_CRED_MAX_LEN];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    if (esp_partition_read(part, sizeof(uint32_t), buf, len) != ESP_OK) {
        return false;
    }
    buf[len] = '\0';

    const char *cursor = buf;
    const char *end = buf + len;

    const char *nl = memchr(cursor, '\n', (size_t)(end - cursor));
    size_t field_len = nl ? (size_t)(nl - cursor) : (size_t)(end - cursor);
    if (field_len == 0) {
        ESP_LOGW(TAG, "wifi_cred has no SSID");
        return false;
    }
    if (field_len >= ssid_cap) {
        field_len = ssid_cap - 1;
    }
    memcpy(ssid_out, cursor, field_len);
    ssid_out[field_len] = '\0';
    if (nl == NULL) {
        return true; // SSID only - no password line (open network) or hostname
    }
    cursor = nl + 1;

    nl = memchr(cursor, '\n', (size_t)(end - cursor));
    field_len = nl ? (size_t)(nl - cursor) : (size_t)(end - cursor);
    if (field_len >= password_cap) {
        field_len = password_cap - 1;
    }
    memcpy(password_out, cursor, field_len);
    password_out[field_len] = '\0';
    if (nl == NULL) {
        return true; // no hostname line
    }
    cursor = nl + 1;

    if (hostname_out != NULL && hostname_cap > 0) {
        nl = memchr(cursor, '\n', (size_t)(end - cursor));
        field_len = nl ? (size_t)(nl - cursor) : (size_t)(end - cursor);
        if (field_len >= hostname_cap) {
            field_len = hostname_cap - 1;
        }
        memcpy(hostname_out, cursor, field_len);
        hostname_out[field_len] = '\0';
    }

    return true;
}

esp_err_t wifi_manager_start(const char *ssid, const char *password, const char *hostname)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (hostname != NULL) {
        esp_err_t hostname_err = esp_netif_set_hostname(s_sta_netif, hostname);
        if (hostname_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set hostname '%s': %s", hostname, esp_err_to_name(hostname_err));
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    memset(&s_wifi_config, 0, sizeof(s_wifi_config));
    strlcpy((char *)s_wifi_config.sta.ssid, ssid, sizeof(s_wifi_config.sta.ssid));
    strlcpy((char *)s_wifi_config.sta.password, password, sizeof(s_wifi_config.sta.password));
    // threshold.authmode is a *minimum* security requirement, not an exact
    // match - WIFI_AUTH_WPA2_WPA3_PSK (the original value here) rejects
    // anything below WPA2/WPA3-transition pre-connection with reason 211
    // (NO_AP_FOUND_IN_AUTHMODE_THRESHOLD). This deployment's AP is an old
    // WPA1-only (WiFi 4) router, so the threshold must be lowered to
    // WPA_PSK to accept it (still accepts WPA2/WPA3 APs too, since those
    // rank higher in wifi_auth_mode_t).
    s_wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    s_wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // Try fast reconnect using cached BSSID + channel + static IP
    wifi_cache_t cache;
    s_fast_connect = false;
    if (nvs_load_cache(&cache) == ESP_OK && cache.channel != 0) {
        ESP_LOGI(TAG, "Fast reconnect: ch=%d BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
                 cache.channel,
                 cache.bssid[0], cache.bssid[1], cache.bssid[2],
                 cache.bssid[3], cache.bssid[4], cache.bssid[5]);
        memcpy(s_wifi_config.sta.bssid, cache.bssid, 6);
        s_wifi_config.sta.bssid_set = true;
        s_wifi_config.sta.channel = cache.channel;
        apply_static_ip(&cache);
        s_fast_connect = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Default STA power-save (WIFI_PS_MIN_MODEM) puts the radio to sleep
    // between each AP beacon and wakes on the AP's DTIM interval (often
    // ~100ms) to check for buffered traffic - during that wake/service
    // window the WiFi driver's own (high-priority) tasks can preempt
    // everything else for a few ms at a time. mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md's
    // follow-up measured exactly that shape on the Host role: an
    // average ~100 HID reports/sec that looked healthy per-second, but a
    // 12us *minimum* gap between two consecutive reports (i.e. several
    // processed back-to-back in a catch-up burst) - consistent with
    // bridge_task periodically being starved for tens of ms at a time
    // and draining a backlog once rescheduled, which would explain a
    // visibly choppy ~10-20Hz cursor despite ~100Hz of data actually
    // flowing. This call disables power-save entirely (radio always on)
    // - higher power draw, but this project already assumes a
    // permanently-powered board, not a battery one.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to '%s'%s...", ssid,
             s_fast_connect ? " (fast reconnect)" : "");

    // Blinks until the first-ever IP_EVENT_STA_GOT_IP (event_handler()
    // above) - a later reconnect after that doesn't blink again, see
    // s_ever_connected's comment.
    status_led_set_blinking(true);

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(void)
{
    // Wait for IP with appropriate timeout
    TickType_t timeout = s_fast_connect
        ? pdMS_TO_TICKS(FAST_CONNECT_TIMEOUT_MS)
        : pdMS_TO_TICKS(30000);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_INIT_DONE_BIT, pdFALSE, pdFALSE, timeout);

    if (bits & WIFI_INIT_DONE_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    // Fast connect failed — fallback to normal scan
    if (s_fast_connect) {
        s_fast_connect = false;
        if (wifi_fallback_connect() == ESP_OK) {
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "WiFi initial connection timed out (will keep retrying in background)");
    return ESP_FAIL;
}

esp_err_t wifi_manager_suspend(void)
{
    s_suspending = true;
    ESP_LOGI(TAG, "Suspending WiFi (USB suspend power saving)");
    return esp_wifi_stop();
}

esp_err_t wifi_manager_resume(void)
{
    ESP_LOGI(TAG, "Resuming WiFi");
    s_suspending = false;
    return esp_wifi_start();
}
