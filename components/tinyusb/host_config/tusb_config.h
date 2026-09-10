#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// KVM_ROLE=HOST's own TinyUSB config - dual rhport, see
// mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md:
// - rhport0 = native OTG as a Device (main/usb_device_typec.c, Phase2
//   type-c output), only actually started when MAX3421E is present.
// - rhport1 = MAX3421E as Host over SPI (main/usb_host_max3421.c),
//   TinyUSB Host mode via hcd_max3421.c, used instead of the native OTG
//   Host path (usb_host_task.c) for devices the native controller
//   mishandles.
// Both halves are always compiled in (KVM_ROLE is a build-time choice,
// this dual-rhport split is not) - main_host.c decides at runtime which
// to actually initialize (usb_host_max3421_probe()).
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

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

// Was missing entirely, silently defaulting to OPT_OS_NONE (tusb_option.h's
// #ifndef fallback) even though this project runs under FreeRTOS. That
// default made TinyUSB's OSAL layer use osal_none.h's "bare metal"
// implementations instead of osal_freertos.h's - in particular,
// osal_none.h's queue (used by tud_task_ext()'s event loop, so on every
// single iteration) locks by disabling/re-enabling the actual USB
// interrupt via esp_intr_alloc()/esp_intr_free() (usbd_int_set() ->
// dcd_int_enable()/dcd_int_disable()) instead of a real RTOS
// primitive - esp_intr_alloc()/free() are one-time setup/teardown calls,
// not designed to be hammered many times a second, and doing so raced
// and double-freed the same handle (`assert failed: tlsf_free ...
// block already marked as free`) - see
// mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md for the full chase (this
// never surfaced with MAX3421E alone since that backend never ran
// tud_task() at all - only once usb_device_typec.c's type-c output
// started actually pumping HID reports did tud_task_ext() run often
// enough to hit it).
#define CFG_TUSB_OS             OPT_OS_FREERTOS

// ── rhport0: Device (type-c output, main/usb_device_typec.c) ──────────
// Single HID interface set (keyboard/mouse/Consumer Control/System
// Control - see main/usb_descriptors.h, reused as-is from the Device
// role). Values mirror what espressif/esp_tinyusb's own Kconfig-driven
// config would produce for that descriptor set - not going through
// esp_tinyusb's Kconfig here since this is a plain compile-time header,
// but usb_device_typec.c *does* still go through esp_tinyusb's
// tinyusb_driver_install() at runtime (see that file for why).
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_HID             4
#define CFG_TUD_HID_EP_BUFSIZE  64
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             0
#define CFG_TUD_DFU             0
#define CFG_TUD_DFU_RUNTIME     0
#define CFG_TUD_BTH             0

// ── rhport1: Host (MAX3421E, main/usb_host_max3421.c) ──────────────────
#define CFG_TUH_MAX3421         1
#define CFG_TUH_HUB             1
#define CFG_TUH_DEVICE_MAX      4

// MAX3421's single physical SIE time-shares "endpoints" in software -
// this sizes that pool, not a hardware channel count like the native
// OTG's OTG_NUM_HOST_CHAN=8. Revisit if a real multi-device test runs
// out, same as happened for the native path
// (mds/usb_hid/2026-08-22_multi_device.md).
#define CFG_TUH_MAX3421_ENDPOINT_TOTAL  16

#define CFG_TUH_HID             8
#define CFG_TUH_HID_EPIN_BUFSIZE   64
#define CFG_TUH_HID_EPOUT_BUFSIZE  64

// CFG_TUH_HID_SET_PROTOCOL_ON_ENUM left at its default (1, hid_host.h) -
// this is the ONLY protocol negotiation usb_host_max3421.c relies on
// (via tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT) there). Adding
// a second, per-device SET_PROTOCOL call on top of this (even carefully
// sequenced after this one's completion callback) reproducibly caused
// the wireless mouse dongle to occasionally serve a wrong-length
// (Boot-shaped) report - see
// mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md. Whatever the exact
// mechanism, empirically: touch this dongle's protocol negotiation only
// once, not twice.

// Not used by this project's Host role.
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_MIDI            0
#define CFG_TUH_VENDOR          0

#endif
