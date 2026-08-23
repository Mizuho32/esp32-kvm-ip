#ifndef USB_HOST_MAX3421_H
#define USB_HOST_MAX3421_H

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST). MAX3421E USB Host path
 * (mds/2026-08-23_filter_conv_router_with_max3421.md): brings up TinyUSB
 * in Host mode over SPI (components/tinyusb's locally-overridden
 * Host+MAX3421 build, see components/tinyusb/CMakeLists.txt and
 * components/tinyusb/host_config/tusb_config.h), parses HID Report
 * Descriptors (hid_report_parser.h), and forwards decoded
 * keyboard/mouse/Consumer Control samples through hid_forwarder.h - the
 * same pipeline usb_host_task.c's native OTG path uses. Confirmed on
 * real hardware to deliver full Report ID-tagged Report Protocol data
 * (see mds/2026-08-22_rp2040_host_check.md's RP2040 cross-test, which
 * this mirrors), so - unlike usb_host_task.c - carries no workaround for
 * the wireless dongle's 3-byte truncation quirk.
 *
 * NOT yet auto-detected/gated behind a fallback to usb_host_task.c's
 * native OTG path (see the md's Phase1 step 4/5 for that) - both run
 * unconditionally side by side for now.
 *
 * Safe to call even with no MAX3421E actually wired up: SPI bus init
 * doesn't require a device to be present, and the background task just
 * logs an initialization failure instead of crashing anything else.
 *
 * @return ESP_OK on success (SPI bus + host task started).
 */
esp_err_t usb_host_max3421_task_start(void);

#endif
