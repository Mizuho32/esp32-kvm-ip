#ifndef DEBUG_STREAM_H
#define DEBUG_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Host role only (KVM_ROLE=HOST). A second, independent esp_http_server
// instance (its own task + listening socket, port DEBUG_STREAM_PORT below)
// that live-streams mruby_filter.c's debug_print() output as
// Server-Sent Events (GET /stream, one line per SSE `data:` frame) to
// whoever's watching the WebUI - see
// mds/usb_hid/2026-09-10_mruby_debug_stream.md for the full design
// rationale (why a *separate* instance/port rather than adding a
// WebSocket/stream endpoint to mruby_webui.c's own httpd, and why it's
// started/stopped on demand rather than left running).
//
// Deliberately NOT started at boot: it costs a dedicated FreeRTOS task
// (stack) plus that task's own listening + client socket buffers for as
// long as it runs - a permanent (not one-off) subtraction from internal
// SRAM, which is already tight once BLE is resident (see
// mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md). mruby_webui.c's
// WebUI exposes explicit Start/Stop buttons (POST /api/debug_stream/start,
// /stop) calling debug_stream_start()/_stop() below - the cost is only
// paid while someone's actually watching.
//
// The internal queue debug_print() pushes into, however, *is* allocated
// once at debug_stream_init() and kept for the process's whole lifetime
// (a small, fixed ~2KB) rather than created/destroyed alongside the httpd
// instance - see debug_stream_push()'s doc comment for why (avoids a
// use-after-free race against a concurrent push from mruby's hot dispatch
// path, at a negligible fixed RAM cost).
#define DEBUG_STREAM_PORT 81

// Call once at boot (Host role only, alongside mruby_webui_start()) -
// allocates the (small, permanent) queue debug_print() pushes into, then
// also resumes the httpd instance itself (via debug_stream_start()) if
// it was left running before the last reboot - see
// mds/usb_hid/2026-09-10_mruby_debug_stream.md's follow-up on why the
// on/off *intent* (not just the live in-RAM state) is persisted to NVS:
// the WebUI's Save/Update-firmware flows already reboot the board on
// every script edit, so without this a debugging session would need
// Start re-clicked after nearly every save.
void debug_stream_init(void);

// True while the httpd instance (started via debug_stream_start()) is up.
// Informational only (mruby_webui.c's /api/status) - nothing else needs
// to branch on this, since debug_stream_push() below already checks it.
bool debug_stream_active(void);

// Non-blocking: pushes one already-formatted line (NUL-terminated,
// truncated internally if too long) into the stream's queue for the
// /stream GET handler to relay onward as an SSE frame. Safe to call
// whether or not the stream is currently started (silently a no-op if
// not, and harmlessly dropped rather than ever blocking if the queue's
// momentarily full while it is) - this is called from
// mruby_filter.c's dsl_debug_print(), which runs on mruby's
// latency-sensitive dispatch path (s_mrb_mutex held) - see that file's
// comment for why blocking here at all is unacceptable.
void debug_stream_push(const char *line);

// Starts the dedicated debug-stream httpd instance (port DEBUG_STREAM_PORT,
// its own task - see this file's top comment) and registers its GET
// /stream handler. Idempotent: a no-op returning ESP_OK if already
// running (e.g. a second browser tab clicking Start, or a reload after
// the stream was left running). Also persists "on" to NVS so
// debug_stream_init() resumes it automatically after the next reboot -
// see that function's doc comment.
esp_err_t debug_stream_start(void);

// Stops the httpd instance started by debug_stream_start() (a no-op
// returning ESP_OK if it isn't running). Also persists "off" to NVS, so
// this is the one call that actually sticks across a reboot - starting
// again always requires an explicit debug_stream_start() (a fresh boot's
// debug_stream_init() only resumes what was last stopped this way, it
// never turns the stream on out of nowhere). Safe to call from
// mruby_webui.c's own httpd worker task despite that being a *different*
// httpd instance's task than the one being stopped - see the .c file's
// comment on why the /stream handler's internal poll loop (rather than
// an unbounded wait) is what makes this not hang the caller (verified
// against esp_http_server's own httpd_stop()/httpd_server() source, not
// just assumed).
esp_err_t debug_stream_stop(void);

// ── Boot-time backlog (separate from the SSE stream above) ─────────────
//
// mruby_filter_init() - and therefore any debug_print() call inside a
// script's top-level source/sink/pipeline code, e.g. an error message a
// script's own `rescue` chose to log rather than let crash the load -
// runs before WiFi even starts, let alone this file's httpd instance
// (which doesn't exist yet at that point, and couldn't be reached over
// the network even if it did). Those lines can never reach the SSE
// stream above no matter how quickly a viewer connects afterward - it's
// not a timing race, the receiving end genuinely doesn't exist yet. See
// mds/usb_hid/2026-09-10_mruby_debug_stream.md's follow-up.
//
// This keeps the last DEBUG_BACKLOG_LINES debug_print() lines (always,
// regardless of debug_print_to's :uart/:http selection - see
// mruby_filter.c's dsl_debug_print()) in a small ring buffer that exists
// from the very first call (lazily allocated, no init() call needed, so
// it's already there during mruby_filter_init() itself), exposed via
// mruby_webui.c's /api/status - fetched unconditionally on every WebUI
// page load, so simply reloading after any reboot (e.g. the Save button's
// automatic reload) shows what a script's earliest debug_print() calls
// said, without needing to catch a live stream or fall back to UART.
#define DEBUG_BACKLOG_LINES    16
#define DEBUG_BACKLOG_LINE_MAX 120

// Always records one line into the backlog ring buffer (oldest dropped
// once full) - unlike debug_stream_push() above, not gated on the httpd
// instance being started (there may not even be a queue yet - see this
// section's doc comment) and never silently a no-op except on the one
// genuine PSRAM allocation failure case. Safe to call from mruby's
// dispatch path (same reasoning as debug_stream_push()): just a memcpy
// into an already-allocated buffer, no I/O, no blocking.
void debug_stream_record_recent(const char *line);

// Renders the backlog (oldest first, one per line, newline-terminated)
// into buf. Returns the number of bytes written (excluding the NUL
// terminator), 0 if nothing has been recorded yet (or the one-time PSRAM
// allocation failed). Used by mruby_webui.c's /api/status handler.
size_t debug_stream_recent_backlog(char *buf, size_t buf_size);

#endif
