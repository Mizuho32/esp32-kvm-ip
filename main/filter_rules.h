#ifndef FILTER_RULES_H
#define FILTER_RULES_H

#include <stdbool.h>
#include <stdint.h>

#include "usb/hid_usage_keyboard.h"

// Host role only (KVM_ROLE=HOST).
//
// Filter/conversion hook, applied to every Boot Protocol report read from
// a physical USB HID device, right before usb_host_task.c packs it into
// the same UDP protocol server.py/protocol.h uses. Edit these functions
// directly to add rules - see mds/2026-08-21_filter_conv_route.md for the
// design rationale and mds/2026-08-21_usb_host.md for background.
//
// IMPORTANT LIMITATION: Boot Protocol reports are a fixed, minimal layout
// (see hid_usage_keyboard.h / hid_usage_mouse.h) - a mouse report is only
// buttons 1-3 + relative X/Y, nothing else. There is no wheel/pan field
// and no button 4/5 (back/forward) in Boot Protocol at all, regardless
// of what the physical mouse actually supports. So "drop the wheel" or
// "remap the back/forward button" can't be written here as-is - that
// data never reaches usb_host_task.c in the first place. Getting it
// would need Report Protocol descriptor parsing (see the "generic"
// report path in usb_host_task.c, currently unimplemented) - a bigger
// task, not done yet.
//
// What IS visible and freely remappable today: keyboard modifiers + up
// to 6 keycodes, and mouse buttons 1-3 + dx/dy. Both functions below
// start as pure passthrough (return true, report untouched); the
// comments show working examples using that data.

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
 * Called for every mouse Boot Protocol report.
 *
 * Note: Boot Protocol mice only report buttons 1-3 and relative X/Y - no
 * wheel/pan, no button 4/5 (back/forward). See the file header comment.
 *
 * @param buttons Bitmask: bit0=button1, bit1=button2, bit2=button3.
 * @param dx      Relative X displacement. Modify in place to remap.
 * @param dy      Relative Y displacement. Modify in place to remap.
 * @return true to forward the (possibly modified) report, false to drop
 *         it entirely.
 */
static inline bool filter_mouse_report(uint8_t *buttons, int8_t *dx, int8_t *dy)
{
    // Example: swap left/right buttons (left-handed mouse):
    //
    // uint8_t left  = *buttons & 0x01;
    // uint8_t right = (*buttons >> 1) & 0x01;
    // *buttons = (*buttons & ~0x03u) | right | (uint8_t)(left << 1);

    // Example: invert vertical movement:
    //
    // *dy = -*dy;

    (void)buttons;
    (void)dx;
    (void)dy;
    return true;
}

// ---------------------------------------------------------------------
// NOT YET POSSIBLE - wheel drop / back-forward -> Alt+arrow
// ---------------------------------------------------------------------
// These were the original motivating examples
// (mds/2026-08-21_filter_conv_route.md), but can't be written as live
// code today: Boot Protocol carries neither the wheel/pan nor buttons
// 4/5 (back/forward) at all (see the file header comment) - the data
// never reaches this file in the first place. Getting it needs Report
// Protocol descriptor parsing (usb_host_task.c's "generic" report path,
// currently unimplemented - not done because it's HID-descriptor-per-
// device-specific, a meaningfully bigger task than everything else
// here). Accepted for now: devices/features Boot Protocol doesn't cover
// are simply not forwarded (makes sense - Boot Protocol itself exists
// to support BIOS/pre-driver environments, so anything past keyboard +
// 3-button mouse was never in scope for it either).
//
// Sketch of what each would look like once Report Protocol parsing
// exists (not compilable as-is - wheel/extra buttons aren't parameters
// of filter_mouse_report() today, and the button-remap case also needs
// the hook to be able to emit an extra keyboard packet, which the
// current one-report-in/one-report-out signature can't do):
//
//   // Drop the wheel (given a hypothetical `int8_t *wheel` param):
//   *wheel = 0;
//
//   // Back/forward -> Alt+Left/Right (mouse event synthesizing a
//   // keyboard event - crosses report types):
//   if (*buttons & MOUSE_BUTTON_BACK) {
//       emit_keyboard_report(HID_LEFT_ALT, HID_KEY_LEFT);
//       *buttons &= ~MOUSE_BUTTON_BACK; // don't also forward as a click
//   }
//   if (*buttons & MOUSE_BUTTON_FORWARD) {
//       emit_keyboard_report(HID_LEFT_ALT, HID_KEY_RIGHT);
//       *buttons &= ~MOUSE_BUTTON_FORWARD;
//   }

#endif
