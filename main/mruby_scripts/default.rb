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

# System Control example - fires a momentary action at whichever sinks
# you name, for a keyboard shortcut this board's physical keyboard has no
# dedicated key for (e.g. the target PC reacts to a hardware Sleep
# button). Not a source/sink/pipeline "kind" like keyboard/mouse/consumer
# above - nothing ever reads this from real hardware, a script only ever
# *sends* it, typically from a :keyboard pipeline watching for some
# chosen combo. See mds/usb_hid/2026-09-10_system_control_sleep.md.
#
# First argument is either a known symbol - :power_down, :sleep,
# :wake_up, :context_menu, :main_menu, :app_menu, :menu_help, :menu_exit,
# :menu_select, :menu_right, :menu_left, :menu_up, :menu_down,
# :cold_restart, :warm_restart - or a raw Integer HID Usage ID in
# 0x81-0x8F for anything else in that range (class/hid/hid.h's
# HID_USAGE_DESKTOP_SYSTEM_*), e.g. `system_control 0x84, :sysctl_typec`
# is the same as `system_control :context_menu, :sysctl_typec`.
#
# 1) declare a sink per output you want it to reach:
#    sink :sysctl_typec, :typec, kind: :system_control
#    sink :sysctl_ble,   :ble,   kind: :system_control
#
# 2) in the `pipeline :keyboard do ... end` block below, replace its
#    `to :typec_kbd` line (currently a plain no-block passthrough) with a
#    block form that watches every report and fires the action as a side
#    effect - swallowing the combo itself (returning nil) so it doesn't
#    also get typed as ordinary keystrokes. Adjust the combo to taste -
#    this example is left-Ctrl+left-Alt+S (modifiers bit0|bit2 = 0x05,
#    keycode 0x16 = S):
#    to(:typec_kbd) { |ev|
#      if ev[:modifiers] & 0x05 == 0x05 && ev[:keycodes].include?(0x16)
#        system_control :sleep, :sysctl_typec, :sysctl_ble
#        next nil
#      end
#      ev
#    }

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

# ble_dynamic true - opt-in (mds/usb_hid/2026-09-11_ble_dynamic_enable.md):
# even with :ble sinks declared above, DON'T auto-start the BLE/NimBLE
# stack at boot (its permanent RAM cost + WiFi-coexistence radio
# contention - see ble_wifi_off_while_connected above - then never
# happens unless actually turned on). The script must then call
# `ble_toggle` itself, typically from a :keyboard pipeline's
# to()/branch() block watching for some chosen combo. `ble_toggle` with
# no argument toggles based on whether the stack is actually running
# right now - no need for the script to track its own on/off guess
# (adjust the combo to taste - this example is left-Ctrl+left-Alt+B,
# modifiers bit0|bit2 = 0x05, keycode 0x05 = B). `ble_toggle(true)`/
# `ble_toggle(false)` are also available if you want an explicit
# set-not-toggle instead.
# ble_dynamic true
#
# pipeline :keyboard do
#   from :local_kbd
#   to(:typec_kbd) { |ev|
#     if ev[:modifiers] & 0x05 == 0x05 && ev[:keycodes].include?(0x05)
#       ble_toggle
#       next nil
#     end
#     ev
#   }
# end

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
