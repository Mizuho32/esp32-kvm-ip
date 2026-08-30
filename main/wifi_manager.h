#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_DISCONNECTED_BIT BIT1

/**
 * Event group signalling WiFi state changes.
 * WIFI_CONNECTED_BIT is set when IP is obtained.
 * WIFI_DISCONNECTED_BIT is set on disconnect (cleared on reconnect).
 */
extern EventGroupHandle_t wifi_event_group;

/**
 * Initializes WiFi in STA mode and starts connecting to the given
 * network, but does NOT wait for it - returns as soon as the netif/WiFi
 * driver are up and the connection attempt has been kicked off (lwIP's
 * TCP/IP thread is alive by the time this returns, which is all
 * socket()/getaddrinfo()/bind() need - see mruby_filter.h). Connecting
 * continues in the background regardless (event_handler's auto-retry-
 * forever loop) - poll wifi_event_group's WIFI_CONNECTED_BIT, or call
 * wifi_manager_wait_connected(), to know when it actually succeeds.
 *
 * @param hostname Sent to the DHCP server (option 12) and used as the
 *                 netif's mDNS-less hostname, so the device can be found
 *                 by name in the router's DHCP lease list. NULL means
 *                 "don't set" (leave the chip's own default hostname
 *                 alone) - see main_host.c/mruby_filter.h for the Host
 *                 role, where this is the mruby script's `hostname` call
 *                 result rather than a compile-time constant, since one
 *                 firmware image is meant to run on multiple boards.
 * @return ESP_OK once the connection attempt has started (this does NOT
 *         mean WiFi is connected yet - see wifi_manager_wait_connected())
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password, const char *hostname);

/**
 * Blocks until WiFi actually connects (WIFI_CONNECTED_BIT), trying a
 * full-scan fallback first if a cached fast-reconnect attempt times out.
 * This is the blocking half wifi_manager_init() used to do as one
 * combined call before it was split - call it right after
 * wifi_manager_start() to get that exact original behavior back.
 *
 * Device role (main.c) does this: its whole job depends on the network
 * (there's no useful "local only" mode for a board that only receives
 * HID over UDP), so it keeps blocking here before doing anything else,
 * same as before the split. Host role (main_host.c) does NOT call this:
 * local USB Host -> type-c input has no WiFi dependency at all, so it
 * proceeds straight from wifi_manager_start() without waiting - the
 * pieces that do need the network (UDP sinks/sources, WebUI) only need
 * the TCP/IP thread wifi_manager_start() already brought up, not a
 * completed AP association, so they don't wait either.
 *
 * @return ESP_OK if connected, ESP_FAIL on timeout (WiFi keeps retrying
 *         in the background regardless, per wifi_manager_start()'s doc)
 */
esp_err_t wifi_manager_wait_connected(void);

/**
 * Stops WiFi (esp_wifi_stop()) for USB-suspend-triggered power saving
 * (see power_manager.c, KVM_ROLE=DEVICE only). Suppresses the normal
 * auto-reconnect-forever handling in this file's event_handler while
 * stopped, so the disconnect event esp_wifi_stop() itself generates
 * doesn't spawn a spurious reconnect attempt. Call wifi_manager_resume()
 * to undo.
 */
esp_err_t wifi_manager_suspend(void);

/**
 * Restarts WiFi after wifi_manager_suspend() (esp_wifi_start()) - the
 * existing WIFI_EVENT_STA_START handler already calls esp_wifi_connect()
 * on start, so this alone reconnects without any extra retry logic.
 */
esp_err_t wifi_manager_resume(void);

#endif
