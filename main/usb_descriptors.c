#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "usb_descriptors.h"
#include "esp_log.h"

static const char *TAG = "USB_DESC";

// ═══════════════════════════════════════════════════════════════════
//  HID REPORT DESCRIPTORS
//  Only consulted in Report Protocol mode - see usb_descriptors.h.
// ═══════════════════════════════════════════════════════════════════

// Keyboard: TinyUSB's stock template already matches the Boot Protocol
// layout (hid_keyboard_report_t), so it's used unchanged in both modes.
static const uint8_t s_hid_report_descriptor_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

// Mouse (Report Protocol mode): 5 buttons, 16-bit X/Y, 8-bit wheel + pan.
// In Boot Protocol mode the firmware instead sends TinyUSB's compact
// hid_mouse_report_t (8-bit X/Y) - this descriptor is never consulted
// then, so the two formats can coexist on the same interface/endpoint.
static const uint8_t s_hid_report_descriptor_mouse[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP      ),
    HID_USAGE      ( HID_USAGE_DESKTOP_MOUSE     ),
    HID_COLLECTION ( HID_COLLECTION_APPLICATION   ),

      HID_USAGE      ( HID_USAGE_DESKTOP_POINTER  ),
      HID_COLLECTION ( HID_COLLECTION_PHYSICAL     ),

        // ── 5 mouse buttons ────────────────────────────────────────
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_BUTTON    ),
        HID_USAGE_MIN   ( 1                         ),
        HID_USAGE_MAX   ( 5                         ),
        HID_LOGICAL_MIN ( 0                         ),
        HID_LOGICAL_MAX ( 1                         ),
        HID_REPORT_COUNT( 5                         ),
        HID_REPORT_SIZE ( 1                         ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),

        // ── 3 padding bits to complete a byte ─────────────────────
        HID_REPORT_COUNT( 1                         ),
        HID_REPORT_SIZE ( 3                         ),
        HID_INPUT       ( HID_CONSTANT              ),

        // ── X, Y: 16-bit relative movement ───────────────────────
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP    ),
        HID_USAGE       ( HID_USAGE_DESKTOP_X       ),
        HID_USAGE       ( HID_USAGE_DESKTOP_Y       ),
        HID_LOGICAL_MIN_N( -32767, 2                ),
        HID_LOGICAL_MAX_N(  32767, 2                ),
        HID_REPORT_SIZE ( 16                         ),
        HID_REPORT_COUNT( 2                          ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),

        // ── Vertical wheel: 8-bit ─────────────────────────────────
        HID_USAGE       ( HID_USAGE_DESKTOP_WHEEL   ),
        HID_LOGICAL_MIN ( -127                       ),
        HID_LOGICAL_MAX (  127                       ),
        HID_REPORT_SIZE ( 8                          ),
        HID_REPORT_COUNT( 1                          ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),

        // ── Horizontal wheel (AC Pan): 8-bit ──────────────────────
        HID_USAGE_PAGE  ( HID_USAGE_PAGE_CONSUMER            ),
        HID_USAGE_N     ( HID_USAGE_CONSUMER_AC_PAN, 2       ),
        HID_LOGICAL_MIN ( -127                                ),
        HID_LOGICAL_MAX (  127                                ),
        HID_REPORT_SIZE ( 8                                   ),
        HID_REPORT_COUNT( 1                                   ),
        HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),

      HID_COLLECTION_END,
    HID_COLLECTION_END,
};

// Consumer Control (media/browser keys) - Report Protocol only, no Boot
// Protocol equivalent exists so BIOS simply never sees this interface.
static const uint8_t s_hid_report_descriptor_consumer[] = {
    TUD_HID_REPORT_DESC_CONSUMER(),
};

// System Control (Power Down/Sleep/Wake Up/menu navigation/restart) -
// Report Protocol only, same as Consumer Control above. Deliberately NOT
// TinyUSB's own TUD_HID_REPORT_DESC_SYSTEM_CONTROL() template (which
// hard-codes only 3 usages via a 2-bit Array field carrying a compressed
// 1/2/3 position index, not the real Usage ID) - hand-written instead,
// same Usage Minimum/Maximum style TUD_HID_REPORT_DESC_CONSUMER() already
// uses above: Logical Minimum == Usage Minimum, so the reported byte
// value equals the actual HID Usage ID directly (0 falls below Logical
// Minimum = idle/none, same convention as Consumer's usage_id). This is
// what lets mruby_filter.c's `system_control` DSL call accept a raw
// Integer usage code, not just a handful of named symbols - see
// SYSTEM_CONTROL_USAGE_MIN/MAX below and
// mds/usb_hid/2026-09-10_system_control_sleep.md's follow-up.
//
// Range chosen: 0x81-0x8F, a contiguous run covering Power Down/Sleep/
// Wake Up plus context/main/app menu navigation and cold/warm restart
// (class/hid/hid.h's HID_USAGE_DESKTOP_SYSTEM_*) - the next usages after
// that (0xA0+: Dock/Undock/Setup/Break/Debugger Break/Speaker Mute/
// Hibernate, 0xB0+: Display Invert/Internal/External/...) sit in
// separate, non-contiguous blocks and are mostly laptop-dock/debugging/
// display-specific rather than generally useful for a KVM shortcut -
// left out for now, extendable later (would need a second Usage Min/Max
// block, or widening this one to include the gap).
#define SYSTEM_CONTROL_USAGE_MIN HID_USAGE_DESKTOP_SYSTEM_POWER_DOWN  // 0x81
#define SYSTEM_CONTROL_USAGE_MAX HID_USAGE_DESKTOP_SYSTEM_WARM_RESTART // 0x8F

static const uint8_t s_hid_report_descriptor_system_control[] = {
    HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP           ),
    HID_USAGE       ( HID_USAGE_DESKTOP_SYSTEM_CONTROL ),
    HID_COLLECTION  ( HID_COLLECTION_APPLICATION       ),
      // Both Logical and Usage Minimum/Maximum encoded as explicit 2-byte
      // items (the _N(..., 2) forms) even though the values fit in a
      // plain byte - 0x81 exceeds the signed 8-bit range (+127), so a
      // 1-byte HID_LOGICAL_MIN/MAX would be misread as a negative value
      // by a strict parser (mirrors s_hid_report_descriptor_mouse's X/Y
      // fields above, which hit the same issue at a larger scale).
      HID_LOGICAL_MIN_N( SYSTEM_CONTROL_USAGE_MIN, 2 ),
      HID_LOGICAL_MAX_N( SYSTEM_CONTROL_USAGE_MAX, 2 ),
      HID_USAGE_MIN_N  ( SYSTEM_CONTROL_USAGE_MIN, 2 ),
      HID_USAGE_MAX_N  ( SYSTEM_CONTROL_USAGE_MAX, 2 ),
      HID_REPORT_COUNT ( 1 ),
      HID_REPORT_SIZE  ( 8 ),
      HID_INPUT        ( HID_DATA | HID_ARRAY | HID_ABSOLUTE ),
    HID_COLLECTION_END,
};

// ═══════════════════════════════════════════════════════════════════
//  DEVICE DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════

tusb_desc_device_t s_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_USB_DESC_VID,
    .idProduct          = CONFIG_USB_DESC_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0,
    .iProduct           = 0,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1,
};

// ═══════════════════════════════════════════════════════════════════
//  CONFIGURATION DESCRIPTOR
// ═══════════════════════════════════════════════════════════════════

#define EPNUM_HID_KEYBOARD 0x81
#define EPNUM_HID_MOUSE    0x82
#define EPNUM_HID_CONSUMER 0x83
#define EPNUM_HID_SYSCTL   0x84
#define HID_POLL_INTERVAL  1

// Mouse endpoint must fit the larger of the two formats it sends
// (7-byte Report-mode mouse_report_t vs 5-byte Boot-mode hid_mouse_report_t).
#define MOUSE_EP_SIZE (sizeof(mouse_report_t) > sizeof(hid_mouse_report_t) \
                       ? sizeof(mouse_report_t) : sizeof(hid_mouse_report_t))

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN * 4)

const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Boot Keyboard interface - bInterfaceSubClass is set to Boot
    // automatically by TUD_HID_DESCRIPTOR() because the protocol
    // argument (HID_ITF_PROTOCOL_KEYBOARD) is non-zero.
    TUD_HID_DESCRIPTOR(ITF_NUM_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report_descriptor_keyboard),
                       EPNUM_HID_KEYBOARD, sizeof(hid_keyboard_report_t),
                       HID_POLL_INTERVAL),

    // Boot Mouse interface
    TUD_HID_DESCRIPTOR(ITF_NUM_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(s_hid_report_descriptor_mouse),
                       EPNUM_HID_MOUSE, MOUSE_EP_SIZE,
                       HID_POLL_INTERVAL),

    // Consumer Control interface - not Boot-capable (protocol = NONE)
    TUD_HID_DESCRIPTOR(ITF_NUM_CONSUMER, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor_consumer),
                       EPNUM_HID_CONSUMER, sizeof(consumer_report_t),
                       HID_POLL_INTERVAL),

    // System Control interface - not Boot-capable (protocol = NONE)
    TUD_HID_DESCRIPTOR(ITF_NUM_SYSCTL, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor_system_control),
                       EPNUM_HID_SYSCTL, sizeof(system_control_report_t),
                       HID_POLL_INTERVAL),
};

// ═══════════════════════════════════════════════════════════════════
//  STRING DESCRIPTORS
// ═══════════════════════════════════════════════════════════════════

const char *s_string_descriptors[4];
uint8_t usb_string_descriptor_count;

void usb_descriptors_init(void)
{
    uint8_t idx = 1;
    s_string_descriptors[0] = "\x09\x04"; // Language: English (US)

    if (CONFIG_USB_DESC_MANUFACTURER[0] != '\0') {
        s_string_descriptors[idx] = CONFIG_USB_DESC_MANUFACTURER;
        s_device_descriptor.iManufacturer = idx++;
    }
    if (CONFIG_USB_DESC_PRODUCT[0] != '\0') {
        s_string_descriptors[idx] = CONFIG_USB_DESC_PRODUCT;
        s_device_descriptor.iProduct = idx++;
    }
    if (CONFIG_USB_DESC_SERIAL[0] != '\0') {
        s_string_descriptors[idx] = CONFIG_USB_DESC_SERIAL;
        s_device_descriptor.iSerialNumber = idx++;
    }

    usb_string_descriptor_count = idx;
}

// ═══════════════════════════════════════════════════════════════════
//  TINYUSB CALLBACKS
// ═══════════════════════════════════════════════════════════════════

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    switch (instance) {
        case ITF_NUM_KEYBOARD: return s_hid_report_descriptor_keyboard;
        case ITF_NUM_MOUSE:    return s_hid_report_descriptor_mouse;
        case ITF_NUM_CONSUMER: return s_hid_report_descriptor_consumer;
        case ITF_NUM_SYSCTL:   return s_hid_report_descriptor_system_control;
        default:                return NULL;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer;   (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)report_id;
    if (instance == ITF_NUM_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT) {
        if (bufsize >= 1) {
            uint8_t leds = buffer[0];
            // leds: bit0=NumLock, bit1=CapsLock, bit2=ScrollLock
            // TODO: Could send UDP back to server or light up an LED on the board
            (void)leds;
        }
    }
}

// Invoked when the host switches Boot <-> Report protocol (SET_PROTOCOL).
// No state to update here: hid_task.c reads tud_hid_n_get_protocol()
// itself at send time to pick the Boot vs Report mouse format.
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    ESP_LOGI(TAG, "itf %u protocol -> %s", instance,
             protocol == HID_PROTOCOL_BOOT ? "BOOT" : "REPORT");
}

// Both weak by default in tinyusb's own usbd.c (only becomes a strong,
// conflicting definition if esp_tinyusb's CONFIG_TINYUSB_SUSPEND_CALLBACK/
// CONFIG_TINYUSB_RESUME_CALLBACK Kconfig options are turned on - they
// aren't, so defining these here is safe, same as the tud_hid_*_cb above).
//
// USB suspend is a bus-level state (host stops SOF traffic for >3ms),
// distinct from VBUS/power presence - this is what actually tells us the
// PC went to sleep/standby, not just "still plugged in". First step
// toward mds/usb_hid/2026-8-30_Sleep.md's power-management work: just
// observe and log for now. s_usb_suspended is exposed via
// usb_descriptors.h for whatever reacts to it next (expected to end up
// behind an mruby DSL toggle rather than hardcoded here, so this stays a
// plain state flag rather than growing sleep logic in this file).
static bool s_usb_suspended;

// power_manager.c is compiled into both roles (see main/CMakeLists.txt),
// but declared weak and WITHOUT a body here regardless - a weak function
// *defined* in this same translation unit would get its calls below
// resolved directly to that local definition at compile time (the "weak"
// attribute only lets the linker pick a strong definition over another
// TU's weak one - it does nothing once the caller's own TU already has a
// body to call), silently shadowing power_manager.c's real definition.
// A bodyless weak extern instead leaves this genuinely unresolved unless
// some other .o defines it, hence the null check below.
extern void power_manager_on_usb_suspend_changed(bool suspended) __attribute__((weak));

void tud_suspend_cb(bool remote_wakeup_en) {
    s_usb_suspended = true;
    ESP_LOGI(TAG, "USB suspended (remote_wakeup_en=%d) - PC likely sleeping/suspended",
             remote_wakeup_en);
    if (power_manager_on_usb_suspend_changed) {
        power_manager_on_usb_suspend_changed(true);
    }
}

void tud_resume_cb(void) {
    s_usb_suspended = false;
    ESP_LOGI(TAG, "USB resumed - PC woke up");
    if (power_manager_on_usb_suspend_changed) {
        power_manager_on_usb_suspend_changed(false);
    }
}

bool usb_device_suspended(void) {
    return s_usb_suspended;
}

void usb_device_force_suspended(void) {
    // tud_suspend_cb() never fires if the PC was *already* suspended
    // before this board booted/connected (no SUSPEND transition for
    // tinyusb to notice - it only sees bus events, not a snapshot of
    // "current" state), so there's a real gap the automatic detection
    // can't cover on its own. This drives the exact same s_usb_suspended
    // flag + power_manager notification tud_suspend_cb() does, so it's a
    // manual stand-in for that missed edge (see mruby_webui.c's /api/sleep -
    // WebUI "Sleep now" button), not a separate mechanism.
    s_usb_suspended = true;
    ESP_LOGI(TAG, "USB suspend forced (manual trigger)");
    if (power_manager_on_usb_suspend_changed) {
        power_manager_on_usb_suspend_changed(true);
    }
}
