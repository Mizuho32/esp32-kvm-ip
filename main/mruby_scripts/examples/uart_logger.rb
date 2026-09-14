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
# with UART) - default port: is 2 (UART_BRIDGE_DEFAULT_PORT, uart_bridge.h),
# the only UART controller this project doesn't already claim (UART0 =
# console, UART1 = usb_host_rp2040_bridge.c's rp2040_bridge backend, if
# that's in use - source(...)/sink(...) raise a clear error at resolve
# time if a `port:` collides with either).
source :dbg_uart, :uart, rx: 4, baud: 115200

# Edit host: to wherever you're running e.g. `nc -ul 9001` to watch the
# log live.
sink :log_pc, :udp, host: "192.168.0.100", port: 9001

pipeline :uart_logger do
  from :dbg_uart
  to :log_pc   # plain passthrough - see below for an example transform
end

# A `to` block on a :uart pipeline receives/returns a String (the raw
# bytes), not a Hash like the other kinds - e.g. to tag every chunk with
# this board's hostname before forwarding:
#
#   to :log_pc do |bytes|
#     "[#{Time.now.strftime('%H:%M:%S')}] #{bytes}"
#   end
#
# Returning nil drops that chunk instead of forwarding it.

# --- Bidirectional / network relay (not enabled above - for reference) -
#
# A `sink :x, :uart, tx: ...` writes back out to a UART TX pin - wire one
# up alongside dbg_uart's rx: to talk to the same peripheral in both
# directions (or a different port: entirely for a separate device):
#
#   sink :dbg_uart_out, :uart, tx: 5, baud: 115200
#
# And a `source :net_in, :udp, listen: 9002` + `from :net_in, kind: :uart`
# pipeline relays bytes arriving over the network back out to a UART TX -
# e.g. sending AT commands to a modem from a PC without a direct cable:
#
#   source :net_in, :udp, listen: 9002
#   pipeline(:uart_inject) { from :net_in, kind: :uart; to :dbg_uart_out }
