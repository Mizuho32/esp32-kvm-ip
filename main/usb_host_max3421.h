#ifndef USB_HOST_MAX3421_H
#define USB_HOST_MAX3421_H

#include "esp_err.h"

/**
 * Host role only (KVM_ROLE=HOST). Phase 1 smoke test for the MAX3421E USB
 * Host path (mds/2026-08-23_filter_conv_router_with_max3421.md): brings up
 * TinyUSB in Host mode over SPI (components/tinyusb's locally-overridden
 * Host+MAX3421 build, see components/tinyusb/CMakeLists.txt and
 * main/tusb_config.h) and just logs whatever HID report descriptors/raw
 * reports show up - the same "dump everything" approach
 * rp2040_host_check/rp2040_host_check.ino used for the RP2040 cross-test.
 *
 * NOT yet wired into hid_report_parser.c/filter_rules.h/protocol.h (the
 * existing UDP-forwarding pipeline), and NOT yet auto-detected/gated
 * behind a fallback to usb_host_task.c's native OTG path (see the md's
 * Phase1 step 4 for that) - this is purely "does a MAX3421E even
 * respond and forward reports correctly on ESP32" verification, meant to
 * run standalone first.
 *
 * Safe to call even with no MAX3421E actually wired up: SPI bus init
 * doesn't require a device to be present, and the background task just
 * sits idle (nothing ever mounts) instead of failing anything else.
 *
 * @return ESP_OK on success (SPI bus + host task started).
 */
esp_err_t usb_host_max3421_task_start(void);

#endif
