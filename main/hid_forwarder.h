#ifndef HID_FORWARDER_H
#define HID_FORWARDER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lwip/sockets.h"

/**
 * Host role only (KVM_ROLE=HOST). Shared forwarding pipeline used by
 * both USB Host backends (native OTG - usb_host_task.c - and MAX3421E/
 * TinyUSB - usb_host_max3421.c, see
 * mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md). Owns the UDP
 * socket, filter_rules.h application, the merged-keyboard-state logic (a
 * physical keyboard's own keys and mouse-triggered synthetic keys - e.g.
 * back/forward -> Alt+arrow - have to be combined into one report, since
 * the UDP protocol carries full state, not deltas - same as server.py's
 * InputState), and (Phase2, MAX3421E backend only) routing between UDP
 * and the type-c USB Device output (usb_device_typec.h): while type-c is
 * connected, every report goes there, and route_rules.h decides whether
 * it *also* gets mirrored over UDP; otherwise everything goes over UDP,
 * same as before Phase2.
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
 * this has worked so far (mds/usb_hid/2026-08-22_consumer_control.md). */
void hid_forwarder_consumer(uint16_t usage_id);

/** Send a report over UDP to an arbitrary destination, not just the
 * fixed KVM_TARGET_HOST every hid_forwarder_*() call above falls back
 * to. Used by mruby_filter.c's DSL `sink :name, :udp, host:, port:` to
 * fan a report out to any number of named UDP targets - see
 * mds/usb_hid/2026-08-29_mruby_phase1_impl.md. Uses the same socket and
 * protocol.h packet format as the rest of this file. */
void hid_forwarder_send_keyboard_to(const struct sockaddr_in *dest, uint8_t modifiers, const uint8_t keycodes[6]);
void hid_forwarder_send_mouse_to(const struct sockaddr_in *dest, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);
void hid_forwarder_send_consumer_to(const struct sockaddr_in *dest, uint16_t usage_id);

/** Send a System Control event (raw HID Usage ID 0x81/0x82/0x83, 0 =
 * idle/release) over UDP to an arbitrary destination - used by
 * mruby_filter.c's `system_control :sleep, ...` DSL call when one of the
 * named sinks is a `sink :name, :udp, ...`. Unlike
 * hid_forwarder_consumer() there's no matching "as if read from local
 * hardware" entry point for this: nothing this project reads from a
 * physical device ever produces a System Control event, so this is only
 * ever reached via mruby_filter.c's direct sink dispatch, never
 * hid_forwarder.c's own per-report paths - see
 * mds/usb_hid/2026-09-10_system_control_sleep.md. */
void hid_forwarder_send_system_control_to(const struct sockaddr_in *dest, uint16_t usage_id);

/** Send a raw byte chunk (protocol.h's raw_bytes_packet_t, not
 * udp_packet_t) to an arbitrary destination - used by mruby_filter.c's
 * `sink :name, :udp` when it's the target of a `:uart`-kind pipeline
 * (mds/usb_hid/2026-09-14_uart_bridge.md). `len` over RAW_BYTES_MAX_LEN
 * is truncated with a warning rather than sent oversized. */
void hid_forwarder_send_raw_bytes_to(const struct sockaddr_in *dest, const uint8_t *data, size_t len);

#endif
