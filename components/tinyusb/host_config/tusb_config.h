#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// KVM_ROLE=HOST's own TinyUSB config (main/usb_host_max3421.c) - TinyUSB
// Host mode over SPI via a MAX3421E, used as a Phase 1 smoke test /
// eventual fallback path alongside the native OTG Host (usb_host_task.c)
// for devices the native controller mishandles - see
// mds/2026-08-23_filter_conv_router_with_max3421.md.
//
// Lives here (inside the tinyusb override component, see
// ../CMakeLists.txt's `target_include_directories(... BEFORE PRIVATE
// "host_config")`) rather than in main/tusb_config.h: espressif/tinyusb
// is unconditionally depended on by main/idf_component.yml regardless of
// KVM_ROLE, but espressif/esp_tinyusb's CMakeLists.txt injects its OWN
// bundled tusb_config.h directly onto the tinyusb library target
// (`target_include_directories(${tusb_lib} PRIVATE "include")`),
// preempting anything in main/ - the `BEFORE` in ../CMakeLists.txt is
// what lets this file win instead, only for KVM_ROLE=HOST.

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUH_MAX3421         1
#define CFG_TUH_HUB             1
#define CFG_TUH_DEVICE_MAX      4

// MAX3421's single physical SIE time-shares "endpoints" in software -
// this sizes that pool, not a hardware channel count like the native
// OTG's OTG_NUM_HOST_CHAN=8. Revisit if a real multi-device test runs
// out, same as happened for the native path
// (mds/2026-08-22_multi_device.md).
#define CFG_TUH_MAX3421_ENDPOINT_TOTAL  16

#define CFG_TUH_HID             8
#define CFG_TUH_HID_EPIN_BUFSIZE   64
#define CFG_TUH_HID_EPOUT_BUFSIZE  64

// Not used by this project's Host role.
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_MIDI            0
#define CFG_TUH_VENDOR          0

#endif
