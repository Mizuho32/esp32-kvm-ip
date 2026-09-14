# Sample mruby script (DSL version): plain passthrough keyboard/mouse/
# consumer pipelines, plus a wireless UART logger - reads bytes off a
# GPIO-wired UART peripheral (e.g. another board's debug console TX pin)
# and relays each chunk to a PC over UDP as they arrive. See
# mds/usb_hid/2026-09-14_uart_bridge.md.
#
# Not built into the firmware image - upload this with:
#   bin/upload_mruby_script.py --port /dev/ttyACM0 \
#       main/mruby_scripts/examples/uart_logger.rb
# then reset the board. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
#
# hostname "esp32-kvm-ip-host"   # uncomment to set - default is unset

source :local_kbd,   :usb_host, kind: :keyboard
source :local_mouse, :usb_host, kind: :mouse
source :local_cc,    :usb_host, kind: :consumer

sink :typec_kbd,   :typec, kind: :keyboard
sink :typec_mouse, :typec, kind: :mouse
sink :typec_cc,    :typec, kind: :consumer

pipeline(:keyboard) { from :local_kbd;   to :typec_kbd }
pipeline(:mouse)    { from :local_mouse; to :typec_mouse }
pipeline(:consumer) { from :local_cc;    to :typec_cc }

# --- UART logger -----------------------------------------------------
#
# rx: is the GPIO reading the far end's TX pin (cross them, as always
# with UART). port: (UART controller number) defaults to
# UART_BRIDGE_DEFAULT_PORT (uart_bridge.h) = 2, the only one this
# project doesn't already claim some use for - shown explicitly here
# anyway since it's worth knowing it's there:
#
#   - port: 0 is always rejected (it's the console, CONFIG_ESP_CONSOLE_UART_NUM)
#   - port: 1 *can* be used, but only warns (doesn't block) if
#     usb_host_rp2040_bridge.c's rp2040_bridge backend is also configured -
#     whichever of the two actually claims it first at boot wins; the
#     other just fails to start cleanly. Pick a different port: (or drop
#     rp2040_bridge from usb_host_backends) if you want both for real.
source :dbg_uart, :uart, rx: 4, baud: 115200, port: 2

# Edit host: to wherever you're running e.g. `nc -ul 9001` to watch the
# log live.
sink :log_pc, :udp, host: "192.168.0.100", port: 9001

pipeline :uart_logger do
  from :dbg_uart
  to :log_pc   # plain passthrough - see below for an example transform
end

# A `to` block on a :uart pipeline receives/returns a String (the raw
# bytes) when the target is a :udp/:uart sink, not a Hash like the other
# kinds - e.g. to tag every chunk with this board's hostname before
# forwarding:
#
#   to :log_pc do |bytes|
#     "[#{Time.now.strftime('%H:%M:%S')}] #{bytes}"
#   end
#
# Returning nil drops that chunk instead of forwarding it. `branch`
# (unlike `to`) can only ever fan the *unmodified* raw bytes out to more
# :udp/:uart sinks - its block only returns true/false, never a value -
# so it's load-time-rejected against a :typec/:ble sink; use `to` (below)
# for those instead.

# --- Synthesizing HID input from UART bytes ---------------------------
#
# A `to` block CAN target a :typec/:ble sink too - return a Hash shaped
# for that sink's own declared `kind:` instead of a String, and it's sent
# as a real keyboard/mouse/consumer/system_control report. E.g. a
# UART-connected macro pad that sends single ASCII digits for a few fixed
# shortcuts:
#
#   HID_KEY_A = 0x04   # see hid_usage_keyboard.h for the full table
#
#   pipeline :uart_macropad do
#     from :dbg_uart
#     to :typec_kbd do |bytes|
#       next nil unless bytes == "a"
#       { modifiers: 0, keycodes: [HID_KEY_A, 0, 0, 0, 0, 0] }
#     end
#   end
#
# A malformed/missing Hash (wrong type, or a :typec/:ble sink declared
# without its own `kind:`) is dropped with a logged warning, not a
# load-time error - only the pipeline *shape* (to/branch vs. sink type)
# is checked when the script loads; a bad value from one particular
# chunk just costs that one chunk.

# --- Bidirectional / network relay (not enabled above - for reference) -
#
# A `sink :x, :uart, tx: ...` writes back out to a UART TX pin - wire one
# up alongside dbg_uart's rx: to talk to the same peripheral in both
# directions (give it the *same* port: so they share one physical UART -
# see uart_bridge_configure()) or a different port: entirely for a
# separate device:
#
#   sink :dbg_uart_out, :uart, tx: 5, baud: 115200, port: 2
#
# And a `source :net_in, :udp, listen: 9002` + `from :net_in, kind: :uart`
# pipeline relays bytes arriving over the network back out to a UART TX -
# e.g. sending AT commands to a modem from a PC without a direct cable:
#
#   source :net_in, :udp, listen: 9002
#   pipeline(:uart_inject) { from :net_in, kind: :uart; to :dbg_uart_out }
