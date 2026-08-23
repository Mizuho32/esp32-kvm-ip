#ifndef ROUTE_RULES_H
#define ROUTE_RULES_H

#include <stdbool.h>
#include <stdint.h>

// Host role only (KVM_ROLE=HOST), MAX3421E backend only.
//
// Default routing hook - pure "type-c only" behavior: when the native
// USB-OTG port is plugged into a PC (type-c output, see
// usb_device_typec.h), a report goes there and, by default, nowhere
// else. This file is tracked in git and is what a fresh checkout builds
// with.
//
// To customize: copy route_rules.h.example to route_rules.h (gitignored,
// like filter_rules.h) and edit it. hid_forwarder.c picks whichever one
// exists (route_rules.h if present, this file otherwise) - see the
// __has_include check there. See
// mds/2026-08-23_filter_conv_router_with_max3421.md's Phase2 section for
// the design rationale.
//
// These run *after* filter_rules.h and only while type-c is actually
// connected (tud_mounted()) - if it isn't, hid_forwarder.c sends
// everything over UDP regardless of these, same as before Phase2. Return
// true to *also* mirror this (already-filtered) report over UDP to the
// existing Device-role board, on top of the type-c output every report
// always gets while connected.

static inline bool route_keyboard_also_udp(uint8_t modifiers, const uint8_t keycodes[6])
{
    (void)modifiers;
    (void)keycodes;
    return false;
}

static inline bool route_mouse_also_udp(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    (void)buttons;
    (void)dx;
    (void)dy;
    (void)wheel;
    (void)pan;
    return false;
}

static inline bool route_consumer_also_udp(uint16_t usage_id)
{
    (void)usage_id;
    return false;
}

#endif
