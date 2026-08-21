#ifndef FILTER_RULES_H
#define FILTER_RULES_H

#include <stdbool.h>
#include <stdint.h>

// Host role only (KVM_ROLE=HOST).
//
// Default filter/conversion hook - pure passthrough, no rules. This file
// is tracked in git and is what a fresh checkout builds with.
//
// To customize: copy filter_rules.h.example to filter_rules.h (gitignored,
// like wifi_credentials.h) and edit it. usb_host_task.c picks whichever
// one exists (filter_rules.h if present, this file otherwise) - see the
// __has_include check in usb_host_task.c. See mds/2026-08-21_filter_conv_route.md
// and mds/2026-08-21_host_report_protocol.md for the design rationale and
// what each parameter means.

static inline bool filter_keyboard_report(uint8_t *modifiers, uint8_t keycodes[6])
{
    (void)modifiers;
    (void)keycodes;
    return true;
}

static inline bool filter_mouse_report(uint8_t *buttons, int16_t *dx, int16_t *dy,
                                       int8_t *wheel, int8_t *pan,
                                       uint8_t *synth_modifiers, uint8_t *synth_keycode)
{
    (void)buttons;
    (void)dx;
    (void)dy;
    (void)wheel;
    (void)pan;
    (void)synth_modifiers;
    (void)synth_keycode;
    return true;
}

#endif
