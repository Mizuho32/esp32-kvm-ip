#ifndef USB_HOST_MAX3421_H
#define USB_HOST_MAX3421_H

#include <stdbool.h>

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
 * Mutually exclusive with usb_host_task.c's native OTG path - see
 * usb_host_max3421_probe() below. main_host.c starts exactly one of the
 * two backends at boot, never both, so that when MAX3421E is present the
 * native OTG peripheral is left free for a future USB Device (type-c)
 * output path - mds/2026-08-23_filter_conv_router_with_max3421.md's
 * Phase2.
 */

/**
 * Probes for a MAX3421E on the configured SPI/GPIO pins (see
 * MAX3421_PIN_* in usb_host_max3421.c): initializes the SPI bus/GPIOs if
 * not already done, then reads the chip's REVISION register directly
 * (bypassing the TinyUSB driver, which isn't initialized yet at this
 * point) and checks it against the known-valid values (0x01/0x12/0x13).
 *
 * Call this once at boot, before deciding whether to start this backend
 * or usb_host_task.c's native OTG backend - see main_host.c. Safe to
 * call with no MAX3421E wired up: just returns false.
 *
 * @return true if a MAX3421E responded, false otherwise.
 */
bool usb_host_max3421_probe(void);

/**
 * Starts the MAX3421E Host backend's background task. Only call this
 * after usb_host_max3421_probe() returned true - see main_host.c.
 *
 * @return ESP_OK on success (host task started).
 */
esp_err_t usb_host_max3421_task_start(void);

#endif
