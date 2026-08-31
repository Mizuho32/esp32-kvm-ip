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

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

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
