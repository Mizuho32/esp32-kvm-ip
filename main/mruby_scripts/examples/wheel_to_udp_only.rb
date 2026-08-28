# Sample mruby script - the original motivating use case for this whole
# mruby effort (see mds/usb_hid/2026-08-28_mruby_filter_route.md's 概要):
# drop the mouse wheel from the type-c output, but still mirror it to the
# Device-role board over UDP. Also ports filter_rules.h.example's
# back/forward -> Alt+Left/Right mapping, to show a keyboard-remap
# example alongside the mouse one.
#
# Not built into the firmware image - upload this with:
#   bin/upload_mruby_script.py --port /dev/ttyUSB0 \
#       main/mruby_scripts/examples/wheel_to_udp_only.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
#
# hostname "esp32-kvm-ip-host"   # uncomment to set - default is noset

HID_LEFT_ALT  = 0x04
HID_KEY_LEFT  = 0x50
HID_KEY_RIGHT = 0x4F

def filter_keyboard(modifiers, keycodes)
  [modifiers, keycodes, true]
end

def filter_mouse(buttons, dx, dy, wheel, pan)
  synth_modifiers = 0
  synth_keycode   = 0

  # Back (button 4, bit 3) -> Alt+Left, Forward (button 5, bit 4) ->
  # Alt+Right. Cleared from `buttons` so they aren't also forwarded as
  # raw button 4/5 clicks. Forward wins if both bits are somehow set.
  if (buttons & (1 << 4)) != 0
    synth_modifiers = HID_LEFT_ALT
    synth_keycode   = HID_KEY_RIGHT
  elsif (buttons & (1 << 3)) != 0
    synth_modifiers = HID_LEFT_ALT
    synth_keycode   = HID_KEY_LEFT
  end
  buttons &= ~((1 << 3) | (1 << 4))

  # The actual point of this script: drop wheel from the type-c copy only
  # (route_mouse_udp below sees the original raw wheel, not this 0).
  wheel = 0

  [buttons, dx, dy, wheel, pan, synth_modifiers, synth_keycode, true]
end

def route_keyboard_udp(modifiers, keycodes)
  false
end

def route_mouse_udp(buttons, dx, dy, wheel, pan)
  # Mirror scroll to UDP - this is the only path wheel/pan reach the
  # Device-role board through, since filter_mouse above drops it from
  # type-c. Sees the *raw* (pre-filter_mouse) values.
  wheel != 0 || pan != 0
end

def route_consumer_udp(usage_id)
  false
end
