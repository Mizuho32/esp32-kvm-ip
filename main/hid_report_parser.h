#ifndef HID_REPORT_PARSER_H
#define HID_REPORT_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Host role only (KVM_ROLE=HOST). See mds/2026-08-21_host_report_protocol.md.
//
// Minimal HID Report Descriptor parser, scoped to what a mouse needs:
// Generic Desktop X/Y/Wheel, Button page buttons, and Consumer AC Pan
// (horizontal scroll). Report Protocol layouts are device-specific
// (unlike Boot Protocol's fixed layout), so this has to be parsed per
// device rather than assumed.
//
// Field byte/bit offsets in a HID report are fully determined by the
// linear order of Input items in the descriptor, regardless of
// Collection nesting - so this walks the descriptor as a flat item
// stream and doesn't need to model the collection hierarchy at all.

#define HID_MAX_BUTTONS 8

// Bit-level location of one field inside a parsed HID input report.
typedef struct {
    bool     present;
    uint8_t  report_id;  // 0 = report has no Report ID prefix byte
    uint16_t bit_offset; // from the start of the report, AFTER any Report ID byte
    uint8_t  bit_length;
    bool     is_signed;
} hid_field_t;

// Field locations discovered by parsing one mouse's Report Protocol HID
// Report Descriptor. Fields default to .present=false if not found.
typedef struct {
    hid_field_t x;
    hid_field_t y;
    hid_field_t wheel;
    hid_field_t pan; // AC Pan (Consumer page) - horizontal scroll
    hid_field_t buttons[HID_MAX_BUTTONS];
    uint8_t     button_count;
} mouse_report_layout_t;

/**
 * Parses `desc` (`desc_len` bytes) for the standard Generic Desktop
 * X/Y/Wheel usages, Button page buttons, and Consumer AC Pan usage.
 * Unrecognized items/usages are silently skipped. Never fails outright -
 * a truncated/malformed descriptor just yields fewer populated fields.
 */
void hid_parse_mouse_report_descriptor(const uint8_t *desc, size_t desc_len,
                                       mouse_report_layout_t *out);

/**
 * Extracts a field's integer value from a raw input report.
 *
 * @param report     Raw report bytes, including any leading Report ID byte.
 * @param report_len Length of `report` in bytes.
 * @param field      Field location from mouse_report_layout_t.
 * @return The field's value (sign-extended if field->is_signed), or 0 if
 *         `field->present` is false, the report is too short, or (when
 *         the field has a Report ID) `report[0]` doesn't match it.
 */
int32_t hid_extract_field(const uint8_t *report, size_t report_len,
                          const hid_field_t *field);

#endif
