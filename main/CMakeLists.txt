# KVM_ROLE selects which board role this build produces: DEVICE (default,
# unchanged behavior - USB HID device connected to the Target PC) or HOST
# (USB Host reading a physical keyboard/mouse, forwarding it over WiFi to
# a Device-role board). Pass -D KVM_ROLE=HOST to idf.py to switch; see
# bin/build_host.sh / bin/build_device.sh and mds/2026-08-21_usb_host.md.
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
            "usb_device_typec.c"
            "usb_descriptors.c"
            "hid_forwarder.c"
            "hid_report_parser.c"
            "wifi_manager.c"
            "status_led.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES
            nvs_flash
            esp_wifi
            esp_netif
            esp_event
            lwip
            esp_driver_gpio
            esp_driver_spi
            usb
    )
elseif(KVM_ROLE STREQUAL "DEVICE")
    idf_component_register(
        SRCS
            "main.c"
            "usb_descriptors.c"
            "wifi_manager.c"
            "network_task.c"
            "hid_task.c"
            "status_led.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES
            nvs_flash
            esp_wifi
            esp_netif
            esp_event
            lwip
            esp_driver_gpio
    )
else()
    message(FATAL_ERROR "Unknown KVM_ROLE '${KVM_ROLE}' (expected HOST or DEVICE)")
endif()
