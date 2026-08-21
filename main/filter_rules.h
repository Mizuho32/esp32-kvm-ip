#ifndef FILTER_RULES_H
#define FILTER_RULES_H

#include <stdbool.h>
#include <stdint.h>

#include "usb/hid_usage_keyboard.h"

// Host role only (KVM_ROLE=HOST).
//
// Filter/conversion hook, applied to every report read from a physical
// USB HID device, right before usb_host_task.c packs it into the same
// UDP protocol server.py/protocol.h uses. Edit these functions directly
// to add rules - see mds/2026-08-21_filter_conv_route.md for the design
// rationale and mds/2026-08-21_host_report_protocol.md for how Report
// vs Boot Protocol selection works on this (Host) side.
//
// Keyboards stay on Boot Protocol (modifiers + up to 6 keycodes - see
// hid_usage_keyboard.h). Mice use Report Protocol when the connected
// device's HID Report Descriptor parses cleanly (giving wheel/pan/extra
// buttons), falling back to Boot Protocol (buttons 1-3 + dx/dy only)
// otherwise - see usb_host_task.c's handle_driver_connected(). Both
// functions start as pure passthrough by default (return true);
// filter_mouse_report() below has the wheel-drop and back/forward ->
// Alt+arrow rules actually enabled, since those were the motivating
// examples for this whole file.

/**
 * Called for every keyboard Boot Protocol report.
 *
 * @param modifiers HID modifier bitmask (HID_LEFT_CONTROL etc. from
 *                  hid_usage_keyboard.h). Modify in place to remap.
 * @param keycodes  Up to 6 simultaneously pressed HID keycodes
 *                  (HID_KEY_* from hid_usage_keyboard.h), 0 = unused slot.
 *                  Modify in place to remap/drop individual keys.
 * @return true to forward the (possibly modified) report, false to drop
 *         it entirely.
 */
static inline bool filter_keyboard_report(uint8_t *modifiers, uint8_t keycodes[6])
{
    // Example: remap Caps Lock -> Left Ctrl. Caps Lock is a keycode slot
    // but Ctrl is a modifier bit, so move it across representations:
    //
    // for (int i = 0; i < 6; i++) {
    //     if (keycodes[i] == HID_KEY_CAPS_LOCK) {
    //         keycodes[i] = HID_KEY_NO_PRESS;
    //         *modifiers |= HID_LEFT_CONTROL;
    //     }
    // }

    // Example: drop a key entirely (e.g. a stuck/annoying key):
    //
    // for (int i = 0; i < 6; i++) {
    //     if (keycodes[i] == HID_KEY_SCROLL_LOCK) {
    //         keycodes[i] = HID_KEY_NO_PRESS;
    //     }
    // }

    (void)modifiers;
    (void)keycodes;
    return true;
}

/**
 * Called for every mouse report (Report Protocol when available, Boot
 * Protocol fallback otherwise - fallback reports always have wheel=0,
 * pan=0, and only buttons 1-3, since that's all Boot Protocol carries).
 *
 * @param buttons Bitmask: bit0=button1, bit1=button2, ... bit(N-1)=buttonN.
 *                By common convention button4=back, button5=forward, but
 *                that's a vendor/OS convention, not guaranteed by the HID
 *                spec itself.
 * @param dx, dy  Relative movement. Modify in place to remap.
 * @param wheel, pan Relative scroll (vertical/horizontal). Modify in
 *                place to remap/drop.
 * @param synth_modifiers, synth_keycode
 *                Output only, zeroed by the caller before this runs. Set
 *                these to merge an extra keyboard key into this cycle's
 *                keyboard state (see usb_host_task.c's
 *                send_merged_keyboard_report()) - e.g. to turn a mouse
 *                button into a keyboard shortcut. This is a level, not
 *                an edge: it's held for as long as the condition below
 *                holds true on each report, mirroring how long the
 *                mouse button itself is held.
 * @return true to forward the (possibly modified) mouse report, false to
 *         drop it entirely. synth_modifiers/synth_keycode apply either way.
 */
static inline bool filter_mouse_report(uint8_t *buttons, int16_t *dx, int16_t *dy,
                                       int8_t *wheel, int8_t *pan,
                                       uint8_t *synth_modifiers, uint8_t *synth_keycode)
{
    // Drop the wheel entirely.
    *wheel = 0;

    // Back (button 4, bit 3) -> Alt+Left, Forward (button 5, bit 4) ->
    // Alt+Right (browser-style back/forward navigation). Cleared from
    // `buttons` so they aren't also forwarded as raw button 4/5 clicks.
    // Forward wins if a mouse somehow reports both bits set at once.
    if (*buttons & (1u << 4)) {
        *synth_modifiers = HID_LEFT_ALT;
        *synth_keycode   = HID_KEY_RIGHT;
    } else if (*buttons & (1u << 3)) {
        *synth_modifiers = HID_LEFT_ALT;
        *synth_keycode   = HID_KEY_LEFT;
    }
    *buttons &= (uint8_t) ~((1u << 3) | (1u << 4));

    // Example: swap left/right buttons (left-handed mouse):
    //
    // uint8_t left  = *buttons & 0x01;
    // uint8_t right = (*buttons >> 1) & 0x01;
    // *buttons = (*buttons & ~0x03u) | right | (uint8_t)(left << 1);

    // Example: invert vertical movement:
    //
    // *dy = -*dy;

    (void)dx;
    (void)pan;
    return true;
}

#endif
