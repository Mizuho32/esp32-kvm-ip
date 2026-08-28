#ifndef MRUBY_FILTER_H
#define MRUBY_FILTER_H

#include <stdbool.h>
#include <stdint.h>

// Host role only (KVM_ROLE=HOST). mruby-scripted alternative to
// filter_rules.h/route_rules.h - see
// mds/usb_hid/2026-08-28_mruby_filter_route.md. Phase1: proves the VM
// embeds, builds, and round-trips a real mrb_funcall() on real hardware;
// the DSL (source/sink/pipeline) described in the design doc is not
// implemented yet - the embedded script only defines the same six hook
// methods filter_rules.h/route_rules.h expose, called directly.
//
// hid_forwarder.c calls mruby_filter_active() once per report to decide
// whether to use these functions or fall back to the C
// filter_rules.h/route_rules.h path - this is the "C fallback switch"
// from the design doc. Active is false whenever CONFIG_MRUBY_FILTER_ROUTE
// is off at build time, or the script failed to load/parse at boot.

void mruby_filter_init(void);
bool mruby_filter_active(void);

// Same contract as filter_rules.h's filter_keyboard_report()/
// filter_mouse_report(), just backed by a call into the loaded script's
// filter_keyboard()/filter_mouse() Ruby methods instead of C.
bool mruby_filter_keyboard_report(uint8_t *modifiers, uint8_t keycodes[6]);
bool mruby_filter_mouse_report(uint8_t *buttons, int16_t *dx, int16_t *dy,
                                int8_t *wheel, int8_t *pan,
                                uint8_t *synth_modifiers, uint8_t *synth_keycode);

// Same contract as route_rules.h's route_*_also_udp().
bool mruby_route_keyboard_also_udp(uint8_t modifiers, const uint8_t keycodes[6]);
bool mruby_route_mouse_also_udp(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);
bool mruby_route_consumer_also_udp(uint16_t usage_id);

// The script may call `hostname "..."` at load time (see main/mruby_scripts/
// default.rb) to set the netif hostname main.c/main_host.c would otherwise
// leave at WIFI_HOSTNAME. Returns NULL if the script never called it -
// callers should leave the chip's default hostname alone in that case
// (default noset, see the design doc's hostname section).
const char *mruby_filter_hostname(void);

#endif
