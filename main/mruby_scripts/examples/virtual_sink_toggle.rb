# Sample mruby script (DSL version): Scroll Lock completely switches
# keyboard/mouse output between a directly-wired Target PC (type-c) and
# a BLE-paired device (phone/tablet/another PC) - never both at once.
# See mds/usb_hid/2026-09-15_virtual_sink.md.
#
# Not built into the firmware image - upload this with:
#   bin/upload_mruby_script.py --port /dev/ttyACM0 \
#       main/mruby_scripts/examples/virtual_sink_toggle.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.

HID_KEY_SCROLL_LOCK = 0x47   # see hid_usage_keyboard.h for the full table

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :ble_kbd,     :ble,   kind: :keyboard
sink :ble_mouse,   :ble,   kind: :mouse

# One virtual sink per kind - each pipeline below wires to its own
# virtual sink exactly once, instead of repeating an
# `if $active == :typec ... else ...` conditional in every `to` block.
sink :vout_kbd,   :virtual, kind: :keyboard
sink :vout_mouse, :virtual, kind: :mouse

# Start pointed at type-c - flip both together so keyboard/mouse never
# end up disagreeing about which output is "active".
def switch_output(target)
  typec = target == :typec
  virtual_sink_set :vout_kbd,   typec ? :typec_kbd   : :ble_kbd
  virtual_sink_set :vout_mouse, typec ? :typec_mouse : :ble_mouse
  debug_print "output -> #{target}"
end
switch_output(:typec)

$scroll_lock_down = false   # press-edge tracking - see the comment below

pipeline :keyboard do
  from :local_kbd

  # Scroll Lock toggles the active output instead of ever reaching a
  # sink itself - matches this whole project's original "Scroll Lock
  # toggles KVM mode" idea, just generalized from Host-PC-vs-Target-PC
  # to type-c-vs-BLE.
  branch :vout_kbd do |ev|
    down = ev[:keycodes].include?(HID_KEY_SCROLL_LOCK)
    if down && !$scroll_lock_down
      # Only toggle on the press *edge* - a held key keeps reporting on
      # every poll (not just once), so checking `down` alone would flip
      # the output back and forth dozens of times while the key is held.
      next_target = virtual_sink_target(:vout_kbd) == :typec_kbd ? :ble : :typec
      switch_output(next_target)
    end
    $scroll_lock_down = down
    !down   # forward everything except while Scroll Lock itself is held
  end
end

pipeline(:mouse) { from :local_mouse; to :vout_mouse }
