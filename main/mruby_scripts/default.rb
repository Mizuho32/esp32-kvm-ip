# Default script (DSL version): pure passthrough, equivalent to
# filter_rules_default.h/route_rules_default.h (type-c only while
# connected, no remap/drop, no UDP mirror). See
# mds/usb_hid/2026-08-28_mruby_filter_route.md (design) and
# mds/usb_hid/2026-08-29_mruby_phase1_impl.md (implementation notes,
# including where this DSL implementation deliberately simplifies the
# original design sketch).
#
# This is embedded into the firmware image at build time (EMBED_TXTFILES,
# main/CMakeLists.txt) - it's the fallback used whenever nothing has been
# uploaded to the mrb_script partition (bin/upload_mruby_script.py).

# hostname "esp32-kvm-ip-host"   # uncomment to set - default is noset

# rp2040_bridge_probe_retries 3        # uncomment to override - default is 3
# rp2040_bridge_probe_timeout_ms 800   # uncomment to override - default is 800

# wifi_reconnect_restart_after 20   # uncomment to override - default is 20, 0 = never restart

# usb_suspend_rp2040_sleep true   # uncomment to enable - default is false (unverified on real
                                  # hardware yet - see mds/usb_hid/2026-08-31_rp2040_sleep_plan.md)

# debug_print_to :uart, :http   # uncomment to override - default is :uart only (:http also
                                # needs the WebUI's "Start debug stream" button clicked to
                                # actually reach a browser - see mds/usb_hid/2026-09-10_mruby_debug_stream.md)

# ntp_sync "pool.ntp.org"   # uncomment to enable - default is disabled (no server set).
                            # SNTP sync once WiFi first connects, so Time.now etc. report the
                            # real date/time instead of a boot-relative duration - see
                            # mds/usb_hid/2026-09-10_ntp_sync.md

# timezone "JST-9"   # uncomment to set - default is unset (UTC). POSIX TZ string (fixed
                     # offset only, no zoneinfo database here - "JST-9" for Japan, not
                     # "Asia/Tokyo"). Time#localtime is a no-op without this (mruby's
                     # Time#localtime takes no arguments, unlike MRI's - see
                     # mds/usb_hid/2026-09-10_ntp_sync.md's follow-up)

# wifi_fast_reconnect_static_ip true   # uncomment to enable - default is false (every boot does a
                                       # real DHCP handshake; enabling this skips it once a cached
                                       # IP exists, which breaks hostname/DNS resolution on the
                                       # router over time - see mds/usb_hid/2026-09-06_wifi_fast_reconnect_static_ip.md)

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

# :ble sinks (mds/usb_hid/2026-09-07_ble_hid_sink_plan.md) - a Bluetooth
# LE HID output straight to the target PC, no wired USB connection to it
# needed. Uncomment to enable (pulls in the NimBLE/esp_hid stack - see
# mruby_filter_ble_sink_declared()). Can run at the same time as the
# :typec sinks above (`to :typec_kbd, :ble_kbd` etc.) - not exclusive.
# sink :ble_kbd,   :ble, kind: :keyboard
# sink :ble_mouse, :ble, kind: :mouse
# sink :ble_cc,    :ble, kind: :consumer

# ble_wifi_off_while_connected true - opt-in (mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
# follow-up): stops WiFi (and therefore the WebUI + any :udp sink)
# entirely for as long as a BLE HID connection is up, freeing the shared
# 2.4GHz radio from WiFi/BT coexistence contention - real-hardware
# testing found this contention still noticeably slows BLE mouse motion
# even with esp_coex biased toward BT. WiFi/WebUI come back automatically
# the moment BLE disconnects.
# ble_wifi_off_while_connected true

pipeline :keyboard do
  from :local_kbd
  to :typec_kbd   # no block = pure passthrough fan-out
end

pipeline :mouse do
  from :local_mouse
  to :typec_mouse
end

pipeline :consumer do
  from :local_cc
  to :typec_cc
end
