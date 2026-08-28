# Phase1 default script: pure passthrough, straight through to UDP/type-c
# exactly like filter_rules_default.h/route_rules_default.h - proves the
# mruby plumbing (VM init, script load, mrb_funcall round trips) works on
# real hardware before any real filter/route DSL logic gets written here.
# See mds/usb_hid/2026-08-28_mruby_filter_route.md.
#
# This is embedded into the firmware image at build time (EMBED_TXTFILES,
# main/CMakeLists.txt) - it is NOT yet loaded from a writable
# partition/WebUI (that's Phase2). Edit this file and reflash to change
# behavior, same as filter_rules.h today.

# hostname "esp32-kvm-ip-host"   # uncomment to set - default is noset
#                                 # (chip's own default hostname is left
#                                 # alone). See mruby_filter.h.

def filter_keyboard(modifiers, keycodes)
  [modifiers, keycodes, true]
end

def filter_mouse(buttons, dx, dy, wheel, pan)
  # buttons, dx, dy, wheel, pan, synth_modifiers, synth_keycode, forward_typec
  [buttons, dx, dy, wheel, pan, 0, 0, true]
end

def route_keyboard_udp(modifiers, keycodes)
  false
end

def route_mouse_udp(buttons, dx, dy, wheel, pan)
  false
end

def route_consumer_udp(usage_id)
  false
end
