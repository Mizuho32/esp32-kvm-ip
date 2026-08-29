# Sample mruby script: keyboard key remapping and a mouse-button
# conditional, showing the basic "if key/button is X, do Y" DSL pattern
# (`to` block operating on the event Hash) - see
# mds/usb_hid/2026-08-28_mruby_filter_route.md's Src/Sink DSL section.
#
# Not built into the firmware image - upload this with:
#   bin/upload_mruby_script.py --port /dev/ttyACM0 \
#       main/mruby_scripts/examples/key_remap.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.

HID_KEY_CAPS_LOCK = 0x39
HID_KEY_LEFT_CTRL = 0xE0

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

pipeline :keyboard do
  from :local_kbd
  to :typec_kbd do |ev|
    # ev[:keycodes] is the 6-key rollover array - remap any occurrence of
    # one keycode to another.
    ev[:keycodes].map! { |k| k == HID_KEY_CAPS_LOCK ? HID_KEY_LEFT_CTRL : k }
    ev
  end
end

pipeline :mouse do
  from :local_mouse
  to :typec_mouse do |ev|
    # ev[:buttons] is a bitmask - use `&` to test individual buttons.
    # Here: holding the side button (button 4, bit 3) suppresses wheel
    # scroll from reaching type-c.
    ev[:wheel] = 0 if (ev[:buttons] & (1 << 3)) != 0
    ev
  end
end

pipeline :consumer do
  from :local_cc
  to :typec_cc
end
