# KVM_ROLE selects which board role this build produces: DEVICE (default,
# unchanged behavior - USB HID device connected to the Target PC) or HOST
# (USB Host reading a physical keyboard/mouse, forwarding it over WiFi to
# a Device-role board). Pass -D KVM_ROLE=HOST to idf.py to switch; see
# bin/build_host.sh / bin/build_device.sh and mds/usb_hid/2026-08-21_usb_host.md.
#
# Deliberately not a Kconfig choice: flipping a Kconfig value regenerates
# sdkconfig.h, which forces a near-full project rebuild. A plain CMake
# variable avoids that - each role is built in its own -B build.<role>
# directory instead, so switching is free.
if(NOT DEFINED KVM_ROLE)
    set(KVM_ROLE "DEVICE")
endif()

if(KVM_ROLE STREQUAL "HOST")
    idf_component_register(
        SRCS
            "main_host.c"
            "usb_host_task.c"
            "usb_host_max3421.c"
            "usb_host_rp2040_bridge.c"
            "usb_device_typec.c"
            "usb_descriptors.c"
            "hid_forwarder.c"
            "hid_report_parser.c"
            "wifi_manager.c"
            "status_led.c"
            "power_manager.c"
            "mruby_filter.c"
            "mruby_webui.c"
        INCLUDE_DIRS "."
        EMBED_TXTFILES
            "mruby_scripts/default.rb"
            "webui/index.html"
        PRIV_REQUIRES
            nvs_flash
            esp_wifi
            esp_netif
            esp_event
            lwip
            esp_driver_gpio
            esp_driver_spi
            esp_timer
            esp_partition
            esp_http_server
            esp_hw_support
            mruby
    )
    # esp_driver_uart (usb_host_rp2040_bridge.c's driver/uart.h), esp_timer
    # (its esp_timer.h, for esp_timer_get_time()), esp_partition
    # (mruby_filter.c's esp_partition.h, for the mrb_script partition - see
    # mds/usb_hid/2026-08-29_mruby_phase1_impl.md), esp_http_server
    # (mruby_webui.c's esp_http_server.h - see
    # mds/usb_hid/2026-08-30_mruby_phase2_webui.md), and mruby (a local
    # non-registry component, mruby_filter.c's mruby.h - see
    # mds/usb_hid/2026-08-28_mruby_filter_route.md) don't get their include
    # dirs propagated to this component via PRIV_REQUIRES above for reasons
    # that didn't resolve after a full clean reconfigure (esp_driver_gpio/
    # esp_driver_spi in the same list work fine) - linked explicitly
    # instead as a workaround.
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_uart idf::esp_timer idf::esp_partition idf::esp_http_server idf::mruby)
elseif(KVM_ROLE STREQUAL "DEVICE")
    idf_component_register(
        SRCS
            "main.c"
            "usb_descriptors.c"
            "wifi_manager.c"
            "network_task.c"
            "hid_task.c"
            "status_led.c"
            "power_manager.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES
            nvs_flash
            esp_wifi
            esp_netif
            esp_event
            lwip
            esp_driver_gpio
            esp_hw_support
    )
else()
    message(FATAL_ERROR "Unknown KVM_ROLE '${KVM_ROLE}' (expected HOST or DEVICE)")
endif()
