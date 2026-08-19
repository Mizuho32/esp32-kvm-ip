#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "usb_descriptors.h"
#include "esp_log.h"

static const char *TAG = "USB_DESC";

// ═══════════════════════════════════════════════════════════════════
//  HID REPORT DESCRIPTORS - one dedicated Boot-capable interface each
//  for Keyboard and Mouse (no Report ID, so Boot/Report protocol send
//  identical bytes - see TUD_HID_REPORT_DESC_* usage below).
// ═══════════════════════════════════════════════════════════════════

static const uint8_t s_hid_report_descriptor_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(),
};

static const uint8_t s_hid_report_descriptor_mouse[] = {
    TUD_HID_REPORT_DESC_MOUSE(),
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
#define HID_POLL_INTERVAL  1

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)

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
                       EPNUM_HID_MOUSE, sizeof(hid_mouse_report_t),
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
// No behavior change needed: both protocols use the same fixed report
// layout (hid_keyboard_report_t / hid_mouse_report_t, no Report ID).
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol) {
    ESP_LOGI(TAG, "itf %u protocol -> %s", instance,
             protocol == HID_PROTOCOL_BOOT ? "BOOT" : "REPORT");
}
