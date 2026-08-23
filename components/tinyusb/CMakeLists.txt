idf_build_get_property(target IDF_TARGET)
string(TOUPPER "OPT_MCU_${target}" tusb_mcu)

set(compile_options
    "-DCFG_TUSB_MCU=${tusb_mcu}"
    )

set(includes_private
    "src/"
    "src/device"
    "lib/networking" # For RNDIS definitions
    )

set(includes_public
    "src/"
    )

# KVM_ROLE=HOST (see ../../main/CMakeLists.txt) needs TinyUSB's Host mode
# + MAX3421E HCD driver (main/usb_host_max3421.c) instead of TinyUSB's
# device-mode sources - see
# mds/2026-08-23_filter_conv_router_with_max3421.md. This is why this
# component is a locally-owned override (main/idf_component.yml's
# override_path) of the espressif/tinyusb registry package instead of the
# fetched copy: the packaged CMakeLists.txt only ever lists device-mode
# sources like the ones in the else() branch below.
#
# Mutually exclusive rather than building both: espressif/esp_tinyusb's
# CMakeLists.txt injects its own bundled (Device-only) tusb_config.h
# directly onto this component's library target regardless of KVM_ROLE
# (`target_include_directories(${tusb_lib} PRIVATE "include")`, executed
# unconditionally since main/idf_component.yml depends on esp_tinyusb
# unconditionally too) - the `target_include_directories(... BEFORE
# PRIVATE "host_config")` below is what makes host_config/tusb_config.h
# win instead for KVM_ROLE=HOST, but that one config header can't
# simultaneously satisfy device-mode files needing CFG_TUD_* macros, so
# this build just doesn't compile those files at all for Host role -
# nothing in the Host role's own code (main_host.c/usb_host_task.c)
# calls into tinyusb's device API anyway.
if(KVM_ROLE STREQUAL "HOST")
    set(srcs
        "src/common/tusb_fifo.c"
        "src/host/usbh.c"
        "src/host/hub.c"
        "src/class/hid/hid_host.c"
        "src/portable/analog/max3421/hcd_max3421.c"
        "src/tusb.c"
        )
else()
    set(srcs
        "src/class/cdc/cdc_device.c"
        "src/class/hid/hid_device.c"
        "src/class/midi/midi_device.c"
        "src/class/midi/midi2_device.c"
        "src/class/msc/msc_device.c"
        "src/class/mtp/mtp_device.c"
        "src/class/printer/printer_device.c"
        "src/class/vendor/vendor_device.c"
        "src/class/audio/audio_device.c"
        "src/class/video/video_device.c"
        "src/class/bth/bth_device.c"
        "src/class/usbtmc/usbtmc_device.c"
        # NET class
        "src/class/net/ecm_rndis_device.c"
        "lib/networking/rndis_reports.c"
        "src/class/net/ncm_device.c"
        # DFU
        "src/class/dfu/dfu_device.c"
        "src/class/dfu/dfu_rt_device.c"
        # Common, device-mode related
        "src/portable/synopsys/dwc2/dcd_dwc2.c"
        "src/portable/synopsys/dwc2/dwc2_common.c"
        "src/common/tusb_fifo.c"
        "src/device/usbd.c"
        "src/tusb.c"
        )
endif()

set(requirements_private
    esp_netif   # required by rndis_reports.c: #include "netif/ethernet.h"
    )

if(${target} STREQUAL "esp32p4")
    list(APPEND requirements_private
                esp_mm # required by dcd_dwc2.c: #include "esp_cache.h"
        )
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes_public}
                       PRIV_INCLUDE_DIRS ${includes_private}
                       PRIV_REQUIRES  ${requirements_private}
                       REQUIRES freertos
                       )

target_compile_options(${COMPONENT_LIB} PUBLIC ${compile_options})

if(KVM_ROLE STREQUAL "HOST")
    # BEFORE so this wins over espressif/esp_tinyusb's own (Device-only)
    # tusb_config.h injection regardless of component processing order -
    # see host_config/tusb_config.h and the block comment above.
    target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "host_config")
else()
    # when no builtin class driver is enabled, an uint8_t data compared with `BUILTIN_DRIVER_COUNT` will always be false
    set_source_files_properties("src/device/usbd.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
endif()
