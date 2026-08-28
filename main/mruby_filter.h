#ifndef MRUBY_FILTER_H
#define MRUBY_FILTER_H

#include <stdbool.h>
#include <stdint.h>

// Host role only (KVM_ROLE=HOST). mruby-scripted source/sink/pipeline DSL
// replacing filter_rules.h/route_rules.h - see
// mds/usb_hid/2026-08-28_mruby_filter_route.md (design) and
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md (what's actually built,
// including where this DSL implementation deliberately simplifies or
// defers parts of the original design sketch - event objects are Hashes
// with symbol keys rather than dot-accessor objects, mouse-triggered
// synthetic keyboard keys are a separate optional `mouse_synth_keys`
// hook rather than a `to` block output, and `:udp` as a *source* - i.e.
// merging the Device role into this same pipeline engine - is not
// implemented).
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

// Resolves every `sink :name, :udp, host:, port:`'s address. Must be
// called once, after WiFi is up (main_host.c, right after
// wifi_manager_init() succeeds) - NOT from mruby_filter_init() itself,
// which runs before WiFi so mruby_filter_hostname() is ready in time,
// but that means lwIP's TCP/IP thread isn't up yet either. Calling
// getaddrinfo() (what this needs) before that crashes real hardware with
// "assert failed: tcpip_send_msg_wait_sem ... Invalid mbox" - see
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md. A no-op if mruby isn't active.
void mruby_filter_resolve_udp_sinks(void);

#endif
