"""
Mapping of Linux evdev KEY_* codes (linux/input-event-codes.h) to HID Usage IDs.

Keyboard/Keypad Page (0x07) — regular keys
Consumer Page (0x0C) — multimedia, browser, and system keys

Targets the same HID usage IDs as hid_keymap.py (Windows VK_*), so both
backends produce identical HID reports for the same physical key.

Source:  USB HID Usage Tables 1.4  https://usb.org/document-library/hid-usage-tables-14
         /usr/include/linux/input-event-codes.h
"""

from evdev import ecodes as e

# ═══════════════════════════════════════════════════════════════════
#  Modifiers - return bit number (0-7) in the modifiers byte
#  bit0=LCtrl  bit1=LShift  bit2=LAlt  bit3=LGUI
#  bit4=RCtrl  bit5=RShift  bit6=RAlt  bit7=RGUI
# ═══════════════════════════════════════════════════════════════════

EV_MODIFIER_MAP: dict[int, int] = {
    e.KEY_LEFTCTRL:  0,
    e.KEY_LEFTSHIFT: 1,
    e.KEY_LEFTALT:   2,
    e.KEY_LEFTMETA:  3,
    e.KEY_RIGHTCTRL: 4,
    e.KEY_RIGHTSHIFT: 5,
    e.KEY_RIGHTALT:  6,
    e.KEY_RIGHTMETA: 7,
}

# ═══════════════════════════════════════════════════════════════════
#  Regular keys - KEY_* code → HID Usage ID (Keyboard/Keypad Page)
# ═══════════════════════════════════════════════════════════════════

EV_TO_HID: dict[int, int] = {

    # ── Letters A-Z ───────────────────────────────────────────────────
    e.KEY_A: 0x04, e.KEY_B: 0x05, e.KEY_C: 0x06, e.KEY_D: 0x07,
    e.KEY_E: 0x08, e.KEY_F: 0x09, e.KEY_G: 0x0A, e.KEY_H: 0x0B,
    e.KEY_I: 0x0C, e.KEY_J: 0x0D, e.KEY_K: 0x0E, e.KEY_L: 0x0F,
    e.KEY_M: 0x10, e.KEY_N: 0x11, e.KEY_O: 0x12, e.KEY_P: 0x13,
    e.KEY_Q: 0x14, e.KEY_R: 0x15, e.KEY_S: 0x16, e.KEY_T: 0x17,
    e.KEY_U: 0x18, e.KEY_V: 0x19, e.KEY_W: 0x1A, e.KEY_X: 0x1B,
    e.KEY_Y: 0x1C, e.KEY_Z: 0x1D,

    # ── Digits 0-9 (top row) ─────────────────────────────────────────
    e.KEY_0: 0x27, e.KEY_1: 0x1E, e.KEY_2: 0x1F, e.KEY_3: 0x20,
    e.KEY_4: 0x21, e.KEY_5: 0x22, e.KEY_6: 0x23, e.KEY_7: 0x24,
    e.KEY_8: 0x25, e.KEY_9: 0x26,

    # ── Function keys F1-F12 ────────────────────────────────────────
    e.KEY_F1: 0x3A, e.KEY_F2: 0x3B, e.KEY_F3: 0x3C, e.KEY_F4: 0x3D,
    e.KEY_F5: 0x3E, e.KEY_F6: 0x3F, e.KEY_F7: 0x40, e.KEY_F8: 0x41,
    e.KEY_F9: 0x42, e.KEY_F10: 0x43, e.KEY_F11: 0x44, e.KEY_F12: 0x45,

    # ── Function keys F13-F24 ───────────────────────────────────────
    e.KEY_F13: 0x68, e.KEY_F14: 0x69, e.KEY_F15: 0x6A, e.KEY_F16: 0x6B,
    e.KEY_F17: 0x6C, e.KEY_F18: 0x6D, e.KEY_F19: 0x6E, e.KEY_F20: 0x6F,
    e.KEY_F21: 0x70, e.KEY_F22: 0x71, e.KEY_F23: 0x72, e.KEY_F24: 0x73,

    # ── Special keys ─────────────────────────────────────────────────
    e.KEY_ENTER:      0x28,
    e.KEY_ESC:        0x29,
    e.KEY_BACKSPACE:  0x2A,
    e.KEY_TAB:        0x2B,
    e.KEY_SPACE:      0x2C,
    e.KEY_CAPSLOCK:   0x39,
    e.KEY_SYSRQ:      0x46,  # Print Screen
    e.KEY_SCROLLLOCK: 0x47,
    e.KEY_PAUSE:      0x48,
    e.KEY_NUMLOCK:    0x53,

    # ── Navigation ────────────────────────────────────────────────────
    e.KEY_INSERT:   0x49,
    e.KEY_HOME:     0x4A,
    e.KEY_PAGEUP:   0x4B,
    e.KEY_DELETE:   0x4C,
    e.KEY_END:      0x4D,
    e.KEY_PAGEDOWN: 0x4E,

    # ── Arrow keys ───────────────────────────────────────────────────
    e.KEY_RIGHT: 0x4F,
    e.KEY_LEFT:  0x50,
    e.KEY_DOWN:  0x51,
    e.KEY_UP:    0x52,

    # ── Numpad ───────────────────────────────────────────────────
    e.KEY_KPSLASH:    0x54,
    e.KEY_KPASTERISK: 0x55,
    e.KEY_KPMINUS:    0x56,
    e.KEY_KPPLUS:     0x57,
    e.KEY_KPENTER:    0x58,
    e.KEY_KP1: 0x59, e.KEY_KP2: 0x5A, e.KEY_KP3: 0x5B, e.KEY_KP4: 0x5C,
    e.KEY_KP5: 0x5D, e.KEY_KP6: 0x5E, e.KEY_KP7: 0x5F, e.KEY_KP8: 0x60,
    e.KEY_KP9: 0x61, e.KEY_KP0: 0x62,
    e.KEY_KPDOT:      0x63,
    e.KEY_KPCOMMA:    0x85,  # Keypad Comma (international)

    # ── Punctuation (US layout) ───────────────────────────────────────
    e.KEY_MINUS:      0x2D,  # - and _
    e.KEY_EQUAL:      0x2E,  # = and +
    e.KEY_LEFTBRACE:  0x2F,  # [ and {
    e.KEY_RIGHTBRACE: 0x30,  # ] and }
    e.KEY_BACKSLASH:  0x31,  # \ and |
    e.KEY_SEMICOLON:  0x33,  # ; and :
    e.KEY_APOSTROPHE: 0x34,  # ' and "
    e.KEY_GRAVE:      0x35,  # ` and ~
    e.KEY_COMMA:      0x36,  # , and <
    e.KEY_DOT:        0x37,  # . and >
    e.KEY_SLASH:      0x38,  # / and ?
    e.KEY_102ND:      0x64,  # Non-US \ and | (key next to LShift on ISO)

    # ── Application / system ──────────────────────────────────────────
    e.KEY_COMPOSE: 0x65,  # Application (Context Menu)
    e.KEY_CANCEL:  0x7B,  # Cancel
    e.KEY_CLEAR:   0x9C,  # Clear
    e.KEY_SELECT:  0x77,  # Select
    e.KEY_HELP:    0x75,  # Help
    e.KEY_POWER:   0x66,  # Power (closest HID mapping, matches Windows VK_SLEEP behavior)

    # ── International keys (IME / Japanese / Korean) ───────────────────
    # Linux exposes these with finer granularity than Windows VK codes.
    e.KEY_RO:                0x87,  # International1 (Ro)
    e.KEY_KATAKANAHIRAGANA:  0x88,  # International2 (Katakana/Hiragana toggle)
    e.KEY_YEN:               0x89,  # International3 (Yen)
    e.KEY_HENKAN:            0x8A,  # International4 (Henkan)
    e.KEY_MUHENKAN:          0x8B,  # International5 (Muhenkan)
    e.KEY_KPJPCOMMA:         0x8C,  # International6 (Keypad Jp Comma)
    e.KEY_HANGEUL:           0x90,  # LANG1 (Hangul/English toggle)
    e.KEY_HANJA:             0x91,  # LANG2 (Hanja)
    e.KEY_KATAKANA:          0x92,  # LANG3 (Katakana)
    e.KEY_HIRAGANA:          0x93,  # LANG4 (Hiragana)

    # Note: VK_ATTN/CRSEL/EXSEL/EREOF/OEM_CLEAR (legacy 3270-terminal era
    # Windows VKs) have no standard evdev equivalent and are intentionally
    # omitted rather than guessed.
}

# ═══════════════════════════════════════════════════════════════════
#  Consumer keys - KEY_* code → HID Consumer Usage ID (Page 0x0C)
#  Sent as a separate Consumer Control HID report.
# ═══════════════════════════════════════════════════════════════════

EV_TO_CONSUMER: dict[int, int] = {

    # ── Volume ───────────────────────────────────────────────────────
    e.KEY_MUTE:       0x00E2,
    e.KEY_VOLUMEDOWN: 0x00EA,
    e.KEY_VOLUMEUP:   0x00E9,

    # ── Media transport ──────────────────────────────────────────────
    e.KEY_NEXTSONG:     0x00B5,
    e.KEY_PREVIOUSSONG: 0x00B6,
    e.KEY_STOPCD:       0x00B7,
    e.KEY_PLAYPAUSE:    0x00CD,

    # ── Browser ──────────────────────────────────────────────────────
    e.KEY_BACK:     0x0224,  # AC Back
    e.KEY_FORWARD:  0x0225,  # AC Forward
    e.KEY_REFRESH:  0x0227,  # AC Refresh
    e.KEY_STOP:     0x0226,  # AC Stop
    e.KEY_SEARCH:   0x0221,  # AC Search
    e.KEY_BOOKMARKS: 0x022A, # AC Bookmarks
    e.KEY_HOMEPAGE: 0x0223,  # AC Home

    # ── Launch applications ──────────────────────────────────────────
    e.KEY_MAIL:  0x018A,  # AL Email Reader
    e.KEY_MEDIA: 0x0183,  # AL Consumer Control Configuration / Media Select
    e.KEY_FILE:  0x0194,  # AL Local Machine Browser
    e.KEY_CALC:  0x0192,  # AL Calculator
}

# ═══════════════════════════════════════════════════════════════════
#  Mapping count verification
# ═══════════════════════════════════════════════════════════════════

_total_mappings = len(EV_MODIFIER_MAP) + len(EV_TO_HID) + len(EV_TO_CONSUMER)
assert _total_mappings >= 100, (
    f"Expected >=100 mappings, got {_total_mappings}"
)
