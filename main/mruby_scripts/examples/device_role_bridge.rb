# Sample mruby script demonstrating a `:udp` source - this Host-role
# board also listens for HID events over UDP (like the separate
# KVM_ROLE=DEVICE role does) and re-emits them via type-c, on top of its
# usual local USB input -> type-c/UDP behavior. See
# mds/usb_hid/2026-08-28_mruby_filter_route.md's Src/Sink DSL section and
# mds/usb_hid/2026-08-29_mruby_phase1_impl.md for how :udp sources work
# (they carry any kind - a separate pipeline per kind, each declaring
# `from :net_in, kind: ...`, is required; contrast with :usb_host sources
# below, whose kind is fixed once at source() time).
#
# Upload with:
#   bin/upload_mruby_script.py --port /dev/ttyACM0 \
#       main/mruby_scripts/examples/device_role_bridge.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
#
# Meant to run with no RP2040 bridge attached - excluding :native_otg here
# keeps native OTG free for type-c device output (without this, main_host.c
# would otherwise fall back to using native OTG as a USB Host input
# backend instead, since neither RP2040 bridge nor MAX3421E is present -
# this board has no physical local input at all, so that fallback would
# just waste the only USB-C port it has for type-c output). Leaving this
# out entirely would still work the same way by default (a :udp source
# with no explicit usb_host_backends already implies this), but it's
# spelled out here since that's exactly the point of this example.
usb_host_backends :rp2040_bridge, :max3421

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer
source :net_in,      :udp, listen: 4210

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

# Local USB input -> type-c only, same as default.rb.
pipeline :keyboard do
  from :local_kbd
  to :typec_kbd
end

pipeline :mouse do
  from :local_mouse
  to :typec_mouse
end

pipeline :consumer do
  from :local_cc
  to :typec_cc
end

# Network-received input (from another board's UDP sink, or server.py)
# also reaches the same type-c output - one pipeline per kind, all
# sourced from the same :net_in.
pipeline :net_keyboard do
  from :net_in, kind: :keyboard
  to :typec_kbd
end

pipeline :net_mouse do
  from :net_in, kind: :mouse
  to :typec_mouse
end

pipeline :net_consumer do
  from :net_in, kind: :consumer
  to :typec_cc
end
