#include "hid_report_parser.h"

#include <string.h>

// HID Report Descriptor item type (bits 3:2 of the item prefix byte).
#define ITEM_TYPE_MAIN   0
#define ITEM_TYPE_GLOBAL 1
#define ITEM_TYPE_LOCAL  2

// Main item tags.
#define TAG_MAIN_INPUT 0x8

// Global item tags.
#define TAG_GLOBAL_USAGE_PAGE      0x0
#define TAG_GLOBAL_LOGICAL_MIN     0x1
#define TAG_GLOBAL_REPORT_SIZE     0x7
#define TAG_GLOBAL_REPORT_ID       0x8
#define TAG_GLOBAL_REPORT_COUNT    0x9
#define TAG_GLOBAL_PUSH            0xA
#define TAG_GLOBAL_POP             0xB

// Local item tags.
#define TAG_LOCAL_USAGE       0x0
#define TAG_LOCAL_USAGE_MIN   0x1
#define TAG_LOCAL_USAGE_MAX   0x2

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_PAGE_BUTTON          0x09
#define USAGE_PAGE_CONSUMER        0x0C
#define USAGE_GENERIC_X            0x30
#define USAGE_GENERIC_Y            0x31
#define USAGE_GENERIC_WHEEL        0x38
#define USAGE_CONSUMER_AC_PAN      0x0238

#define MAX_GLOBAL_STACK   4
#define MAX_REPORT_CURSORS 4
#define MAX_PENDING_USAGES 16

typedef struct {
    uint16_t usage_page;
    int32_t  logical_minimum;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t  report_id;
} global_state_t;

typedef struct {
    uint8_t  report_id;
    uint16_t bit_cursor;
} report_cursor_t;

static report_cursor_t *find_or_create_cursor(report_cursor_t *cursors, int *count, uint8_t report_id)
{
    for (int i = 0; i < *count; i++) {
        if (cursors[i].report_id == report_id) {
            return &cursors[i];
        }
    }
    if (*count >= MAX_REPORT_CURSORS) {
        return NULL;
    }
    report_cursor_t *c = &cursors[(*count)++];
    c->report_id = report_id;
    c->bit_cursor = 0;
    return c;
}

static void maybe_record_field(mouse_report_layout_t *out, uint16_t page, uint16_t usage,
                               uint8_t report_id, uint16_t bit_offset, uint8_t bit_length,
                               bool is_signed)
{
    hid_field_t field = {
        .present     = true,
        .report_id   = report_id,
        .bit_offset  = bit_offset,
        .bit_length  = bit_length,
        .is_signed   = is_signed,
    };

    if (page == USAGE_PAGE_GENERIC_DESKTOP && usage == USAGE_GENERIC_X && !out->x.present) {
        out->x = field;
    } else if (page == USAGE_PAGE_GENERIC_DESKTOP && usage == USAGE_GENERIC_Y && !out->y.present) {
        out->y = field;
    } else if (page == USAGE_PAGE_GENERIC_DESKTOP && usage == USAGE_GENERIC_WHEEL && !out->wheel.present) {
        out->wheel = field;
    } else if (page == USAGE_PAGE_CONSUMER && usage == USAGE_CONSUMER_AC_PAN && !out->pan.present) {
        out->pan = field;
    } else if (page == USAGE_PAGE_BUTTON && usage >= 1 && usage <= HID_MAX_BUTTONS) {
        field.is_signed = false;
        out->buttons[usage - 1] = field;
        if (usage > out->button_count) {
            out->button_count = (uint8_t)usage;
        }
    }
}

// Called for every bit-slot of every non-constant Main Input item, in
// descriptor order - `usage`/`have_usage` are only meaningful for slots
// that resolved to a specific Local Usage (single Usage item, a matching
// Usage Minimum/Maximum range, or one Usage item shared by every slot);
// an array selector field (e.g. Consumer Control's "current usage ID"
// field) has no per-slot usage of its own, so have_usage is false there.
typedef void (*input_slot_cb_t)(void *ctx, uint16_t usage_page, uint32_t usage, bool have_usage,
                                uint8_t report_id, uint16_t bit_offset, uint8_t bit_length,
                                bool is_signed);

static void walk_report_descriptor(const uint8_t *desc, size_t desc_len,
                                   input_slot_cb_t cb, void *ctx)
{
    global_state_t g = {0};
    global_state_t stack[MAX_GLOBAL_STACK];
    int stack_depth = 0;

    uint32_t pending_usages[MAX_PENDING_USAGES];
    int pending_usage_count = 0;
    bool have_usage_range = false;
    uint32_t usage_range_min = 0, usage_range_max = 0;

    report_cursor_t cursors[MAX_REPORT_CURSORS];
    int cursor_count = 0;

    size_t i = 0;
    while (i < desc_len) {
        uint8_t item = desc[i++];

        if (item == 0xFE) { // Long item (rare) - skip entirely.
            if (i + 2 > desc_len) {
                break;
            }
            uint8_t data_size = desc[i];
            i += 2 + data_size;
            continue;
        }

        uint8_t size_code = item & 0x03;
        uint8_t type      = (item >> 2) & 0x03;
        uint8_t tag       = (item >> 4) & 0x0F;
        uint8_t data_len  = (size_code == 3) ? 4 : size_code;

        if (i + data_len > desc_len) {
            break; // Truncated - stop, keep whatever was found so far.
        }

        uint32_t udata = 0;
        for (uint8_t b = 0; b < data_len; b++) {
            udata |= ((uint32_t)desc[i + b]) << (8 * b);
        }
        int32_t sdata;
        if (data_len == 1) {
            sdata = (int8_t)udata;
        } else if (data_len == 2) {
            sdata = (int16_t)udata;
        } else {
            sdata = (int32_t)udata;
        }
        i += data_len;

        if (type == ITEM_TYPE_GLOBAL) {
            switch (tag) {
                case TAG_GLOBAL_USAGE_PAGE:  g.usage_page = (uint16_t)udata; break;
                case TAG_GLOBAL_LOGICAL_MIN: g.logical_minimum = sdata; break;
                case TAG_GLOBAL_REPORT_SIZE: g.report_size = udata; break;
                case TAG_GLOBAL_REPORT_ID:   g.report_id = (uint8_t)udata; break;
                case TAG_GLOBAL_REPORT_COUNT: g.report_count = udata; break;
                case TAG_GLOBAL_PUSH:
                    if (stack_depth < MAX_GLOBAL_STACK) {
                        stack[stack_depth++] = g;
                    }
                    break;
                case TAG_GLOBAL_POP:
                    if (stack_depth > 0) {
                        g = stack[--stack_depth];
                    }
                    break;
                default: break;
            }
            continue;
        }

        if (type == ITEM_TYPE_LOCAL) {
            switch (tag) {
                case TAG_LOCAL_USAGE:
                    if (pending_usage_count < MAX_PENDING_USAGES) {
                        pending_usages[pending_usage_count++] = udata;
                    }
                    break;
                case TAG_LOCAL_USAGE_MIN:
                    usage_range_min = udata;
                    have_usage_range = true;
                    break;
                case TAG_LOCAL_USAGE_MAX:
                    usage_range_max = udata;
                    have_usage_range = true;
                    break;
                default: break;
            }
            continue;
        }

        // Main item (Input/Output/Feature/Collection/End Collection).
        if (tag == TAG_MAIN_INPUT) {
            report_cursor_t *cur = find_or_create_cursor(cursors, &cursor_count, g.report_id);
            if (cur) {
                bool is_const = (udata & 0x01) != 0; // Input item flags bit 0 = Constant.
                for (uint32_t slot = 0; slot < g.report_count; slot++) {
                    uint32_t usage_full = 0;
                    bool have_usage = false;

                    if (pending_usage_count > 0 && (uint32_t)pending_usage_count == g.report_count) {
                        usage_full = pending_usages[slot];
                        have_usage = true;
                    } else if (have_usage_range && (usage_range_min + slot) <= usage_range_max) {
                        usage_full = usage_range_min + slot;
                        have_usage = true;
                    } else if (pending_usage_count == 1) {
                        usage_full = pending_usages[0];
                        have_usage = true;
                    }

                    if (!is_const) {
                        uint16_t page  = (have_usage && usage_full > 0xFFFF) ? (uint16_t)(usage_full >> 16) : g.usage_page;
                        uint16_t usage = (uint16_t)usage_full;
                        uint16_t bit_off = (uint16_t)(cur->bit_cursor + slot * g.report_size);
                        cb(ctx, page, usage, have_usage, g.report_id, bit_off,
                          (uint8_t)g.report_size, g.logical_minimum < 0);
                    }
                }
                cur->bit_cursor = (uint16_t)(cur->bit_cursor + g.report_size * g.report_count);
            }
        }

        // Local state is cleared after every Main item (spec-mandated),
        // regardless of which kind of Main item it was.
        pending_usage_count = 0;
        have_usage_range = false;
    }
}

static void mouse_input_slot_cb(void *ctx, uint16_t page, uint32_t usage, bool have_usage,
                                uint8_t report_id, uint16_t bit_offset, uint8_t bit_length,
                                bool is_signed)
{
    if (!have_usage) {
        return;
    }
    maybe_record_field((mouse_report_layout_t *)ctx, page, (uint16_t)usage, report_id,
                       bit_offset, bit_length, is_signed);
}

void hid_parse_mouse_report_descriptor(const uint8_t *desc, size_t desc_len,
                                       mouse_report_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    walk_report_descriptor(desc, desc_len, mouse_input_slot_cb, out);
}

static void consumer_input_slot_cb(void *ctx, uint16_t page, uint32_t usage, bool have_usage,
                                   uint8_t report_id, uint16_t bit_offset, uint8_t bit_length,
                                   bool is_signed)
{
    (void)is_signed;
    consumer_report_layout_t *out = (consumer_report_layout_t *)ctx;

    // First non-constant Consumer-page field wide enough to hold a real
    // usage ID (rules out 1-bit-per-key "bitmap" fields, which this
    // doesn't support - see mds/2026-08-22_consumer_control.md). Keep
    // only the first match; a descriptor with more than one such field
    // is unusual and not worth guessing between.
    if (out->selector.present || page != USAGE_PAGE_CONSUMER || bit_length < 8) {
        return;
    }
    // AC Pan (horizontal scroll) also lives on the Consumer page but
    // isn't a "currently pressed key" selector - real hardware has been
    // seen bundling it into an unrelated Mouse-usage sub-report (a
    // different Report ID) on the very same physical HID interface as
    // the real Consumer Control selector, so it has to be excluded by
    // name rather than just by width - see
    // mds/2026-08-22_consumer_control.md.
    if (have_usage && usage == USAGE_CONSUMER_AC_PAN) {
        return;
    }
    out->selector = (hid_field_t){
        .present    = true,
        .report_id  = report_id,
        .bit_offset = bit_offset,
        .bit_length = bit_length,
        .is_signed  = false, // Usage IDs are always unsigned.
    };
}

void hid_parse_consumer_report_descriptor(const uint8_t *desc, size_t desc_len,
                                          consumer_report_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    walk_report_descriptor(desc, desc_len, consumer_input_slot_cb, out);
}

int32_t hid_extract_field(const uint8_t *report, size_t report_len, const hid_field_t *field)
{
    if (!field->present) {
        return 0;
    }

    size_t byte_offset0 = 0;
    if (field->report_id != 0) {
        if (report_len < 1 || report[0] != field->report_id) {
            return 0;
        }
        byte_offset0 = 1;
    }

    uint32_t abs_bit = (uint32_t)(byte_offset0 * 8) + field->bit_offset;
    uint32_t needed_bits = abs_bit + field->bit_length;
    if (needed_bits > report_len * 8) {
        return 0;
    }

    uint32_t value = 0;
    for (uint8_t b = 0; b < field->bit_length; b++) {
        uint32_t bitnum = abs_bit + b;
        uint8_t byte = report[bitnum / 8];
        uint8_t bit = (byte >> (bitnum % 8)) & 1;
        value |= ((uint32_t)bit) << b;
    }

    if (field->is_signed && field->bit_length < 32) {
        uint32_t sign_bit = 1u << (field->bit_length - 1);
        if (value & sign_bit) {
            value |= ~((sign_bit << 1) - 1);
        }
    }

    return (int32_t)value;
}
