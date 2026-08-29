# Sample mruby script (DSL version) - the original motivating use case for
# this whole mruby effort (see mds/usb_hid/2026-08-28_mruby_filter_route.md's
# 概要): drop the mouse wheel from the type-c output, but still mirror it
# to the Device-role board over UDP. Also ports the back/forward ->
# Alt+Left/Right mapping (filter_rules.h.example's example) via the
# mouse_synth_keys hook.
#
# Not built into the firmware image - upload this with:
#   bin/upload_mruby_script.py --port /dev/ttyACM0 \
#       main/mruby_scripts/examples/wheel_to_udp_only.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
#
# hostname "esp32-kvm-ip-host"   # uncomment to set - default is noset

HID_LEFT_ALT  = 0x04
HID_KEY_LEFT  = 0x50
HID_KEY_RIGHT = 0x4F

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

# Edit host: to your Device-role board's IP (KVM_TARGET_HOST equivalent -
# unlike that constant this is just one more named sink, so more can be
# added for fan-out to multiple boards).
sink :main_pc, :udp, host: "192.168.0.100", port: 4210
sink :sub_pc,  :udp, host: "192.168.1.101", port: 4210

# Independent of the pipeline below - see mruby_filter.h and
# mds/usb_hid/2026-08-29_mruby_phase1_impl.md for why this is a separate
# hook rather than a `to` block output.
def mouse_synth_keys(buttons, dx, dy, wheel, pan)
  # Back (button 4, bit 3) -> Alt+Left, Forward (button 5, bit 4) ->
  # Alt+Right (browser-style back/forward navigation). Forward wins if a
  # mouse somehow reports both bits set at once.
  return [HID_LEFT_ALT, HID_KEY_RIGHT] if (buttons & (1 << 4)) != 0
  return [HID_LEFT_ALT, HID_KEY_LEFT]  if (buttons & (1 << 3)) != 0
  [0, 0]
end

pipeline :keyboard do
  from :local_kbd
  to :typec_kbd
end

pipeline :mouse do
  from :local_mouse

  # type-c only: drop wheel, and clear the back/forward button bits
  # (mouse_synth_keys above already turned them into Alt+arrow, so they
  # shouldn't also show up as raw button 4/5 clicks).
  to :typec_mouse do |ev|
    ev[:buttons] &= ~((1 << 3) | (1 << 4))
    ev[:wheel] = 0
    ev
  end

  # Raw (pre-filter) values - this is the only path wheel/pan reach the
  # Device-role board through, since the `to` stage above drops wheel
  # from type-c.
  branch(:main_pc) { |ev| ev[:wheel] != 0 || ev[:pan] != 0 }
  branch(:sub_pc) { |ev| ev[:wheel] != 0 || ev[:pan] != 0 }
end

pipeline :consumer do
  from :local_cc
  to :typec_cc
end
