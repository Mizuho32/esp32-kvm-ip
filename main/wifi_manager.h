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
 * Initializes WiFi in STA mode and connects to the given network.
 * Blocks until an IP address is obtained.
 * WiFi will auto-reconnect indefinitely on disconnect.
 *
 * @param hostname Sent to the DHCP server (option 12) and used as the
 *                 netif's mDNS-less hostname, so the device can be found
 *                 by name in the router's DHCP lease list. NULL means
 *                 "don't set" (leave the chip's own default hostname
 *                 alone) - see main_host.c/mruby_filter.h for the Host
 *                 role, where this is the mruby script's `hostname` call
 *                 result rather than a compile-time constant, since one
 *                 firmware image is meant to run on multiple boards.
 * @return ESP_OK if connected, ESP_FAIL on failure
 */
esp_err_t wifi_manager_init(const char *ssid, const char *password, const char *hostname);

#endif
