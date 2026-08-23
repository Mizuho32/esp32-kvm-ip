#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// NOTE: this file is not actually reachable by the tinyusb component's
// own compilation - espressif/esp_tinyusb's CMakeLists.txt does
// `target_include_directories(${tusb_lib} PRIVATE "include")` on the
// tinyusb library target directly, injecting ITS OWN bundled
// include/tusb_config.h ahead of anything here (confirmed via
// compile_commands.json - no -I flag ever points at this directory for
// any tinyusb source file). The Device role's real CFG_TUD_* values come
// from that file (Kconfig-driven, CONFIG_TINYUSB_*) instead. Kept here
// only because a plain `#include "tusb_config.h"` still requires SOME
// file to exist by that name for anything that includes tusb.h directly;
// its actual #define content is inert. See
// components/tinyusb/host_config/tusb_config.h for where the Host role's
// (KVM_ROLE=HOST) TinyUSB Host + MAX3421E config actually lives instead
// (mds/2026-08-23_filter_conv_router_with_max3421.md).

// Role: Device
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

// Endpoint 0 size
#define CFG_TUD_ENDPOINT0_SIZE  64

// Enabled device classes
#define CFG_TUD_HID             3   // Boot Keyboard (itf 0) + Boot Mouse (itf 1) + Consumer Control (itf 2)
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// HID endpoint buffer (>= largest report: mouse_report_t = 7 bytes)
#define CFG_TUD_HID_EP_BUFSIZE  16

#endif
