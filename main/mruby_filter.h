#ifndef MRUBY_FILTER_H
#define MRUBY_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Host role only (KVM_ROLE=HOST). mruby-scripted source/sink/pipeline DSL
// replacing filter_rules.h/route_rules.h - see
// mds/usb_hid/2026-08-28_mruby_filter_route.md (design) and
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md (what's actually built,
// including where this DSL implementation deliberately simplifies the
// original design sketch - event objects are Hashes with symbol keys
// rather than dot-accessor objects, and mouse-triggered synthetic
// keyboard keys are a separate optional `mouse_synth_keys` hook rather
// than a `to` block output). `:udp` sources are implemented (a script
// declares one with `source :name, :udp, listen: PORT`), as is script
// control over which physical USB Host backend(s) main_host.c tries via
// `usb_host_backends(*syms)` - see mruby_filter_host_backend_count()/_at()
// below.
//
// hid_forwarder.c calls mruby_filter_active() once per report to decide
// whether to use these functions or fall back to the C
// filter_rules.h/route_rules.h path - this is the "C fallback switch"
// from the design doc. Active is false whenever CONFIG_MRUBY_FILTER_ROUTE
// is off at build time, or the script failed to load/parse at boot.

void mruby_filter_init(void);
bool mruby_filter_active(void);

// Runs the merged/current keyboard state through the script's :keyboard
// pipeline (every `to`/`branch` stage, dispatching to whichever sinks
// they reference - typec and/or any named UDP sinks). Always sends;
// hid_forwarder.c's caller already decided type-c is connected (mruby
// pipelines are typec-connected-only, same scope as the old route_rules.h).
void mruby_dispatch_keyboard(uint8_t modifiers, const uint8_t keycodes[6]);

// Same for a mouse sample - also fills *synth_modifiers/*synth_keycode
// from the script's optional top-level `mouse_synth_keys(buttons, dx, dy,
// wheel, pan) -> [modifiers, keycode]` method (0/HID_KEY_NO_PRESS if the
// script doesn't define one), mirroring filter_rules.h's synth_modifiers/
// synth_keycode output parameters - see hid_forwarder.c's
// apply_mouse_synth_keys().
void mruby_dispatch_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan,
                          uint8_t *synth_modifiers, uint8_t *synth_keycode);

void mruby_dispatch_consumer(uint16_t usage_id);

// The script may call `hostname "..."` at load time (see main/mruby_scripts/
// default.rb) to set the netif hostname main.c/main_host.c would otherwise
// leave at WIFI_HOSTNAME. Returns NULL if the script never called it -
// callers should leave the chip's default hostname alone in that case
// (default noset, see the design doc's hostname section).
const char *mruby_filter_hostname(void);

// The script may call `usb_suspend_wifi_sleep false` to opt this board
// out of power_manager.c's reaction to its PC's USB link suspending
// (stopping WiFi, cycling light sleep, dimming the status LED - see
// mds/usb_hid/2026-8-30_Sleep.md). Defaults to true (enabled) if the
// script never calls it. power_manager.c looks this up via a weak-symbol
// reference (this file isn't compiled into KVM_ROLE=DEVICE builds at
// all, which just get the default true).
bool mruby_filter_usb_suspend_wifi_sleep_enabled(void);

// Resolves every `sink :name, :udp, host:, port:`'s address. Must be
// called once, after WiFi is up (main_host.c, right after
// wifi_manager_init() succeeds) - NOT from mruby_filter_init() itself,
// which runs before WiFi so mruby_filter_hostname() is ready in time,
// but that means lwIP's TCP/IP thread isn't up yet either. Calling
// getaddrinfo() (what this needs) before that crashes real hardware with
// "assert failed: tcpip_send_msg_wait_sem ... Invalid mbox" - see
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md. A no-op if mruby isn't active.
void mruby_filter_resolve_udp_sinks(void);

// Starts the `source :name, :udp, listen: PORT` receive task, if the
// loaded script declared one (no-op otherwise, or if mruby isn't active).
// Same WiFi-must-be-up ordering constraint as mruby_filter_resolve_udp_sinks()
// - call it right alongside that, after wifi_manager_init() succeeds.
// Received HID events are dispatched through a *separate* set of
// kind-indexed pipelines from local (:usb_host-sourced) ones - a script
// distinguishes them via `from :net_in, kind: :mouse` (required for :udp
// sources, since unlike a physical mouse this source carries any kind)
// vs. plain `from :local_mouse` for local input. See
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
void mruby_filter_start_net_source(void);

// Which physical USB Host backend(s) main_host.c should try, and in what
// order - fully controlled by the script's `usb_host_backends(*syms)`
// call (e.g. `usb_host_backends :rp2040_bridge, :max3421` to exclude
// native OTG, freeing it for type-c device output). If the script never
// calls it, this defaults to [:rp2040_bridge, :max3421] when a `:udp`
// source is declared (that board has no use for native-OTG-as-host, and
// needs type-c actually started for its network-sourced pipelines'
// :typec sinks to work) or [:rp2040_bridge, :max3421, :native_otg]
// otherwise (today's original hardcoded order). main_host.c tries each
// in turn, stopping at the first one that actually starts; if none do
// (empty list, or the list is exhausted without a :native_otg entry),
// native OTG is left free for type-c device output. If mruby isn't
// active at all (VM failed to open, or both the uploaded script and the
// embedded default.rb failed to parse), count() is 0 and main_host.c
// instead runs its original, fully hardcoded pure-C probe order
// unconditionally - this is the "fall back to pure C" mruby init-failure
// path. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
typedef enum {
    MRUBY_HOST_BACKEND_RP2040_BRIDGE,
    MRUBY_HOST_BACKEND_MAX3421,
    MRUBY_HOST_BACKEND_NATIVE_OTG,
} mruby_host_backend_t;

int mruby_filter_host_backend_count(void);
mruby_host_backend_t mruby_filter_host_backend_at(int index);

// Reads the currently active script's raw source - the uploaded
// mrb_script partition content if present, else the embedded
// main/mruby_scripts/default.rb - into buf, NUL-terminated (truncated
// if buf_size is too small). Works even if mruby failed to activate
// (only touches the partition/embedded bytes, not the VM). Used by
// mruby_webui.c (Phase 2, see mds/usb_hid/2026-08-30_mruby_phase2_webui.md)
// to prefill its edit page. Returns the number of bytes written,
// excluding the NUL.
size_t mruby_filter_read_script(char *buf, size_t buf_size);

// The exact byte length mruby_filter_read_script() would report (without
// the NUL, and without truncation) - i.e. how big a buffer to allocate
// for it. Reads only the 4-byte length header (or uses the embedded
// default.rb's compile-time size), no content read/allocation.
size_t mruby_filter_script_len(void);

// Overwrites the mrb_script partition with new_script (new_len bytes,
// not required to be NUL-terminated) - the same on-flash format
// bin/upload_mruby_script.py writes (4-byte little-endian length +
// UTF-8 source). Does not reload the running VM: mruby_webui.c calls
// esp_restart() after a successful write so the new script takes effect
// from a clean boot, same as the serial-upload workflow. Returns
// ESP_ERR_NOT_FOUND if the partition doesn't exist, ESP_ERR_INVALID_SIZE
// if new_len doesn't fit it.
esp_err_t mruby_filter_write_script(const char *new_script, size_t new_len);

#endif
