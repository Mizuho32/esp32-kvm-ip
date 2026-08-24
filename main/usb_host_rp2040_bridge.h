#ifndef USB_HOST_RP2040_BRIDGE_H
#define USB_HOST_RP2040_BRIDGE_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST). Alternative USB Host backend to
 * usb_host_max3421.c - see
 * mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md. Instead of an SPI-
 * attached MAX3421E SIE + TinyUSB Host running on this chip, an external
 * RP2040 does the actual USB Host role itself (its own native USB
 * peripheral + TinyUSB Host, already proven on real hardware via
 * rp2040_host_check/ to receive full, untruncated HID reports from the
 * same wireless dongle that ESP32-S3's native OTG Host mishandles), and
 * forwards already-decoded HID mount/report events to this board over a
 * plain UART link (rp2040_host_bridge/rp2040_host_bridge.ino).
 *
 * Motivation: the MAX3421E SPI protocol requires precisely-timed
 * multi-byte transactions held across a single CS assertion, which
 * turned out to be sensitive to breadboard wiring quality (mount/unmount
 * instability, occasional 0-byte reports, never fully resolved - see
 * mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md). A UART link is
 * just a byte stream (self-clocked per byte, no shared multi-byte
 * transaction timing), so it's expected to tolerate wiring quality much
 * better - and since the RP2040 does its own HID report descriptor
 * parsing, this board's TinyUSB Host stack (hcd_max3421.c, usbh.c,
 * hid_host.c) isn't needed at all for this backend - only
 * hid_report_parser.c and hid_forwarder.c, both already
 * transport-independent.
 *
 * Same role in main_host.c as usb_host_max3421_probe()/task_start(): a
 * runtime-probed, mutually-exclusive alternative to native OTG (and to
 * MAX3421E) - whichever backend actually responds wins. Also implies the
 * native OTG port is free, same as the MAX3421E case, so
 * usb_device_typec_start() applies here too.
 */

/**
 * Probes for an RP2040 bridge on the configured UART pins (see
 * BRIDGE_UART_* in usb_host_rp2040_bridge.c): initializes the UART if
 * not already done, then listens for a validated (checksummed) frame -
 * the RP2040 side sends a periodic heartbeat frame regardless of USB
 * device state, so any valid frame is strong evidence of a real RP2040
 * speaking this protocol, not line noise.
 *
 * Call this once at boot, before deciding which Host backend to start -
 * see main_host.c. Safe to call with nothing wired up: just times out
 * and returns false.
 *
 * @return true if a validated frame was received within the timeout.
 */
bool usb_host_rp2040_bridge_probe(void);

/**
 * Starts the bridge's background UART-reader task. Only call this after
 * usb_host_rp2040_bridge_probe() returned true - see main_host.c.
 *
 * @return ESP_OK on success (task started).
 */
esp_err_t usb_host_rp2040_bridge_task_start(void);

#endif
