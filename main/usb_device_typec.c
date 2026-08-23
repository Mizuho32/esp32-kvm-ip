#include "usb_device_typec.h"

#include <string.h>

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "class/hid/hid_device.h"
#include "usb_descriptors.h"

#define TAG "USBDEV_TYPEC"

static bool s_started;

static int8_t clamp_to_i8(int16_t v)
{
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int8_t)v;
}

esp_err_t usb_device_typec_start(void)
{
    // Same call main.c's Device role uses (espressif/esp_tinyusb's
    // tinyusb_driver_install(), PHY init + descriptor wiring + its own
    // background tud_task() loop, all in one). Safe to reuse here even
    // though this is otherwise a TinyUSB Host-mode build
    // (CFG_TUH_MAX3421 etc. - see
    // components/tinyusb/host_config/tusb_config.h): esp_tinyusb is a
    // separate component from tinyusb itself, and which tud_*/tuh_*
    // entry points get *called* at runtime has no bearing on which
    // tusb_config.h *compiled* tinyusb's own sources - that's already
    // decided at build time by components/tinyusb/CMakeLists.txt's
    // `BEFORE` override, independent of this. In particular,
    // tud_descriptor_device_cb() and friends live in esp_tinyusb's own
    // descriptors_control.c (always compiled into every role - see
    // main/idf_component.yml), populated only by this call - writing our
    // own copies of those callbacks instead would collide (duplicate
    // symbol) with esp_tinyusb's, so this must go through
    // tinyusb_driver_install() rather than a hand-rolled PHY+tud_rhport_init()
    // like usb_host_max3421.c does for the Host side.
    usb_descriptors_init();
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = TINYUSB_TASK_DEFAULT(),
        .descriptor = {
            .device            = &s_device_descriptor,
            .string            = s_string_descriptors,
            .string_count      = usb_string_descriptor_count,
            .full_speed_config = s_configuration_descriptor,
        },
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install() failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "USB Device (type-c) started - waiting for a PC to enumerate it...");
    return ESP_OK;
}

bool usb_device_typec_connected(void)
{
    return s_started && tud_mounted();
}

// Debug aid - see usb_host_rp2040_bridge.c's/usb_host_max3421.c's
// equivalent raw-report dump toggles. Logs why a report got dropped
// (not connected vs. endpoint not ready are different failure modes -
// the former means tud_mounted() is false, the latter means the host
// hasn't picked up the previous report yet) or, if it was actually
// attempted, whether tud_hid_n_report() itself reports success.
// Comment back out once confirmed stable.
static bool log_send_precheck(const char *what, uint8_t itf_num)
{
    bool connected = usb_device_typec_connected();
    bool ready = tud_hid_n_ready(itf_num);
    if (!connected || !ready) {
        ESP_LOGW(TAG, "%s dropped: connected=%d ready=%d", what, connected, ready);
        return false;
    }
    return true;
}

void usb_device_typec_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    if (!log_send_precheck("keyboard report", ITF_NUM_KEYBOARD)) {
        return;
    }
    hid_keyboard_report_t report = {
        .modifier = modifiers,
        .reserved = 0x00,
    };
    memcpy(report.keycode, keycodes, 6);
    bool sent = tud_hid_n_report(ITF_NUM_KEYBOARD, 0, &report, sizeof(report));
    ESP_LOGI(TAG, "keyboard report sent=%d modifiers=0x%02x", sent, modifiers);
}

void usb_device_typec_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    if (!log_send_precheck("mouse report", ITF_NUM_MOUSE)) {
        return;
    }
    // Mirrors hid_task.c: Boot Protocol (BIOS/bootloader) gets the fixed
    // 8-bit compact layout, Report Protocol (OS loaded) gets the full
    // 16-bit relative layout.
    bool sent;
    if (tud_hid_n_get_protocol(ITF_NUM_MOUSE) == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t report = {
            .buttons = buttons,
            .x       = clamp_to_i8(dx),
            .y       = clamp_to_i8(dy),
            .wheel   = wheel,
            .pan     = pan,
        };
        sent = tud_hid_n_report(ITF_NUM_MOUSE, 0, &report, sizeof(report));
    } else {
        mouse_report_t report = {
            .buttons = buttons,
            .x       = dx,
            .y       = dy,
            .wheel   = wheel,
            .pan     = pan,
        };
        sent = tud_hid_n_report(ITF_NUM_MOUSE, 0, &report, sizeof(report));
    }
    ESP_LOGI(TAG, "mouse report sent=%d buttons=0x%02x dx=%d dy=%d", sent, buttons, dx, dy);
}

void usb_device_typec_consumer_report(uint16_t usage_id)
{
    if (!log_send_precheck("consumer report", ITF_NUM_CONSUMER)) {
        return;
    }
    consumer_report_t report = {
        .usage_id = usage_id,
    };
    bool sent = tud_hid_n_report(ITF_NUM_CONSUMER, 0, &report, sizeof(report));
    ESP_LOGI(TAG, "consumer report sent=%d usage_id=0x%04x", sent, usage_id);
}
