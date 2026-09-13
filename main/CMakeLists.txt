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
            "esp_hid_gap.c"
            "nimble_hidd_fork.c"
            "ble_pair_slots.c"
            "ble_hid_device.c"
            "mruby_ctype_shim.c"
            "mruby_alloc_psram.c"
            "debug_stream.c"
            "ota_updater.c"
            "heap_monitor.c"
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
            bt
            esp_hid
            esp_coex
            app_update
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
    # instead as a workaround. bt/esp_hid (esp_hid_gap.c/ble_hid_device.c's
    # NimBLE headers) has the same include-propagation issue - and also
    # needs to be listed *before* idf::mruby here (not after), or the
    # final link command's placement of libmruby.a shifts such that it
    # can no longer resolve against libc (undefined reference to
    # __getreent/_ctype_ from mruby's own object files) - mruby needs to
    # stay the last of this explicit list, same position it already had
    # before bt/esp_hid existed.
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_uart idf::esp_timer idf::esp_partition idf::esp_http_server idf::bt idf::esp_hid idf::esp_coex idf::mruby)
    # __getreent: libmruby.a ends up as the very last archive on the
    # final link command (true even before bt/esp_hid existed) -
    # normally harmless, since __getreent (which mruby's Prism parser
    # needs) gets pulled from esp_libc earlier via some other
    # force-included symbol. Adding bt/esp_hid to the mix somehow
    # disturbs whichever earlier reference used to do that incidentally
    # (undefined reference to __getreent appeared only once these were
    # added) - force it in explicitly instead of relying on that
    # incidental ordering.
    #
    # _ctype_: a *separate* undefined reference (mruby-sprintf gem's
    # isspace()/isdigit()/etc., which this toolchain's ctype.h apparently
    # expands to a classic `_ctype_[c+1]`-table lookup when mruby's own
    # Rakefile-driven cross-compile step builds it - unlike ESP-IDF's own
    # C files, which never hit that code path). No archive in this
    # toolchain/esp_libc build defines `_ctype_` at all (confirmed - not
    # even the toolchain's own libc.a), so it can't be force-included the
    # same way - see mruby_ctype_shim.c for the actual fix (a dummy
    # all-zero table; harmless since no script here calls Kernel#sprintf/
    # format with anything ctype-sensitive).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,__getreent")
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
            esp_partition
            esp_timer
            app_update
    )
    # esp_partition (wifi_manager.c's esp_partition.h, for the wifi_cred
    # partition - see wifi_manager_load_credentials()) and esp_timer
    # (status_led.c's esp_timer.h, for the WiFi-connecting blink - see
    # status_led_set_blinking()) don't get their include dirs propagated
    # via PRIV_REQUIRES for the same unresolved reason as the HOST role's
    # own esp_partition/esp_timer/esp_driver_uart workaround below - linked
    # explicitly instead.
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_partition idf::esp_timer)
else()
    message(FATAL_ERROR "Unknown KVM_ROLE '${KVM_ROLE}' (expected HOST or DEVICE)")
endif()
