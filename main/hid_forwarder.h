#ifndef HID_FORWARDER_H
#define HID_FORWARDER_H

#include <stdint.h>
#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST). Shared UDP-forwarding pipeline used by
 * both USB Host backends (native OTG - usb_host_task.c - and MAX3421E/
 * TinyUSB - usb_host_max3421.c, see
 * mds/2026-08-23_filter_conv_router_with_max3421.md). Owns the UDP
 * socket, filter_rules.h application, and the merged-keyboard-state
 * logic (a physical keyboard's own keys and mouse-triggered synthetic
 * keys - e.g. back/forward -> Alt+arrow - have to be combined into one
 * report, since the UDP protocol carries full state, not deltas - same
 * as server.py's InputState).
 *
 * Each backend only does its own device enumeration/report parsing -
 * once a sample is decoded (buttons/dx/dy/wheel/pan, modifiers/keycodes,
 * or a Consumer usage ID), hand it to one of these.
 *
 * Must be called after WiFi is connected (wifi_manager_init()), and
 * hid_forwarder_init() must run before either backend starts.
 */

esp_err_t hid_forwarder_init(void);

/** A keyboard's Boot Protocol state (modifiers + 6-key rollover),
 * already extracted from whatever raw report struct the calling
 * backend uses - merged with any synthetic keys and sent. */
void hid_forwarder_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

/** A fully-decoded mouse sample (buttons/dx/dy/wheel/pan), regardless of
 * whether it came from Report or Boot Protocol. Runs it through
 * filter_rules.h and sends it on. */
void hid_forwarder_mouse_sample(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

/** A Consumer Control ("media keys", or a mouse's bundled
 * volume/forward/back selector) usage ID, forwarded as-is on every call
 * (0 = release) - not run through filter_rules.h (yet), matching how
 * this has worked so far (mds/2026-08-22_consumer_control.md). */
void hid_forwarder_consumer(uint16_t usage_id);

#endif
