#ifndef _UART_BRIDGE_H_
#define _UART_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Backs mruby_filter.c's `source :name, :uart, rx:, ...` / `sink :name,
// :uart, tx:, ...` DSL (mds/usb_hid/2026-09-14_uart_bridge.md) - lets a
// script wire an arbitrary GPIO-pair UART up to the same source/sink/
// pipeline machinery keyboard/mouse/consumer/system_control already use,
// carrying raw byte chunks instead of a fixed HID report (e.g. a
// wireless UART logger: `source :dbg, :uart, rx: 4; sink :pc, :udp,
// host: ...; pipeline { from :dbg; to :pc }`).
//
// UART2 is the only ESP32-S3 UART controller this project doesn't
// already claim: UART0 is the console (CONFIG_ESP_CONSOLE_UART_NUM),
// UART1 is usb_host_rp2040_bridge.c's BRIDGE_UART_PORT (only actually in
// use when that USB Host backend is selected, but mruby_filter.c's
// mruby_filter_resolve_uart_bridges() rejects it either way to keep this
// header independent of that file's own #define). Default port is
// overridable per source/sink via `port:` since which UART ends up free
// is a per-deployment/wiring question, not something this project can
// hardcode for every board.
#define UART_BRIDGE_DEFAULT_PORT 2
#define UART_BRIDGE_DEFAULT_BAUD 115200
#define UART_BRIDGE_PIN_UNUSED   (-1)

// Configures (or extends) one UART peripheral. Idempotent per port and
// safe to call once for a `source :x, :uart, rx: ...` declaration and
// once (separately) for a `sink :y, :uart, tx: ...` one on the same
// port - mruby_filter_resolve_uart_bridges() does exactly that, in
// whichever order the script declared them. Pass UART_BRIDGE_PIN_UNUSED
// for whichever of rx_pin/tx_pin this call doesn't know about (it won't
// overwrite a pin set by an earlier call for the same port). Returns
// ESP_ERR_INVALID_STATE if a later call supplies a different baud than
// an already-configured one for that port - the earlier baud wins, the
// mismatched declaration is left unconfigured.
esp_err_t uart_bridge_configure(int port, int rx_pin, int tx_pin, int baud);

// Starts the RX task for `port` - a no-op if that port has no rx_pin
// configured (TX-only/write-side ports never need one) or was already
// started. Bytes read off the wire are handed to mruby_dispatch_uart_rx()
// (mruby_filter.h) in whatever-sized chunks uart_read_bytes() returns,
// each becoming one pipeline dispatch - see uart_bridge.c.
void uart_bridge_start_rx(int port);

// Writes to `port`'s TX pin - used by mruby_filter.c's
// send_uart_bytes_to_sink() for a `sink :name, :uart` target. Logs a
// warning and drops the data if that port has no tx_pin configured.
void uart_bridge_write(int port, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
