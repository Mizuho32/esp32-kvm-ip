#include "usb_host_task.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "protocol.h"
#include "hid_report_parser.h"
#include "wifi_credentials.h"

// filter_rules.h is a gitignored personal copy of filter_rules.h.example
// (like wifi_credentials.h) - fall back to the tracked, pure-passthrough
// default if it hasn't been created.
#if __has_include("filter_rules.h")
#include "filter_rules.h"
#else
#include "filter_rules_default.h"
#endif

#define TAG "USBHOST"

// HID host driver events (device connect/disconnect) are delivered from
// the driver's own background task and just get queued here; the actual
// per-report handling (hid_host_interface_callback below) runs directly
// on that background task instead, since it only does cheap work
// (filter + UDP send), matching how the official
// examples/peripherals/usb/host/hid example structures this.
static QueueHandle_t s_driver_event_queue;

static int s_sock = -1;
static struct sockaddr_in s_target_addr;
static uint32_t s_seq;

// Merged keyboard state: physical keyboard (handle_keyboard_report) and
// mouse-triggered synthetic keys (e.g. back/forward -> Alt+arrow, see
// filter_rules.h) both contribute to one combined report, the same way
// server.py's InputState merges keyboard + paste-typing on the Windows
// side. Without this, whichever source sent last would silently
// overwrite the other's held keys (the UDP protocol carries full state,
// not deltas).
static uint8_t s_kbd_modifiers;
static uint8_t s_kbd_keycodes[6];
static uint8_t s_synth_modifiers;
static uint8_t s_synth_keycode = HID_KEY_NO_PRESS;

// Per-device Report Protocol layout, for mice whose HID Report
// Descriptor parsed cleanly (see mds/2026-08-21_host_report_protocol.md).
// Devices that don't parse (or aren't Boot Interface subclass, so no
// fallback exists) are simply not tracked here and their reports ignored.
#define MAX_MOUSE_DEVICES 4
typedef struct {
    hid_host_device_handle_t handle;
    bool                     use_report_protocol;
    mouse_report_layout_t    layout;
} mouse_device_state_t;
static mouse_device_state_t s_mouse_devices[MAX_MOUSE_DEVICES];
static int s_mouse_device_count;

static mouse_device_state_t *find_mouse_device(hid_host_device_handle_t handle)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].handle == handle) {
            return &s_mouse_devices[i];
        }
    }
    return NULL;
}

static mouse_device_state_t *register_mouse_device(hid_host_device_handle_t handle)
{
    if (s_mouse_device_count >= MAX_MOUSE_DEVICES) {
        return NULL;
    }
    mouse_device_state_t *d = &s_mouse_devices[s_mouse_device_count++];
    memset(d, 0, sizeof(*d));
    d->handle = handle;
    return d;
}

static void unregister_mouse_device(hid_host_device_handle_t handle)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].handle == handle) {
            s_mouse_devices[i] = s_mouse_devices[--s_mouse_device_count];
            return;
        }
    }
}

// Per-device Consumer Control ("media keys") layout - see
// mds/2026-08-22_consumer_control.md. Devices whose Report Descriptor
// doesn't yield a recognizable selector field are closed again right
// away in handle_driver_connected() and never reach this table.
#define MAX_CONSUMER_DEVICES 4
typedef struct {
    hid_host_device_handle_t handle;
    consumer_report_layout_t layout;
} consumer_device_state_t;
static consumer_device_state_t s_consumer_devices[MAX_CONSUMER_DEVICES];
static int s_consumer_device_count;

static consumer_device_state_t *find_consumer_device(hid_host_device_handle_t handle)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].handle == handle) {
            return &s_consumer_devices[i];
        }
    }
    return NULL;
}

static consumer_device_state_t *register_consumer_device(hid_host_device_handle_t handle)
{
    if (s_consumer_device_count >= MAX_CONSUMER_DEVICES) {
        return NULL;
    }
    consumer_device_state_t *d = &s_consumer_devices[s_consumer_device_count++];
    memset(d, 0, sizeof(*d));
    d->handle = handle;
    return d;
}

static void unregister_consumer_device(hid_host_device_handle_t handle)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].handle == handle) {
            s_consumer_devices[i] = s_consumer_devices[--s_consumer_device_count];
            return;
        }
    }
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool resolve_target(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", UDP_PORT);

    int err = getaddrinfo(KVM_TARGET_HOST, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "Failed to resolve KVM_TARGET_HOST '%s': %d", KVM_TARGET_HOST, err);
        return false;
    }
    memcpy(&s_target_addr, res->ai_addr, sizeof(s_target_addr));
    freeaddrinfo(res);
    return true;
}

static void send_udp_packet(const udp_packet_t *pkt)
{
    if (s_sock < 0) {
        return;
    }
    sendto(s_sock, pkt, PACKET_SIZE, 0, (struct sockaddr *)&s_target_addr, sizeof(s_target_addr));
}

static void send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_KEYBOARD,
    };
    pkt.keyboard.modifiers = modifiers;
    pkt.keyboard.reserved  = 0;
    memcpy(pkt.keyboard.keycodes, keycodes, 6);
    send_udp_packet(&pkt);
}

static void send_mouse_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_MOUSE,
    };
    pkt.mouse.buttons = buttons;
    pkt.mouse.dx      = dx;
    pkt.mouse.dy      = dy;
    pkt.mouse.wheel   = wheel;
    pkt.mouse.pan     = pan;
    send_udp_packet(&pkt);
}

static void send_consumer_report(uint16_t usage_id)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_CONSUMER,
    };
    pkt.consumer.usage_id = usage_id;
    send_udp_packet(&pkt);
}

static void send_merged_keyboard_report(void)
{
    uint8_t modifiers = s_kbd_modifiers | s_synth_modifiers;
    uint8_t keycodes[6];
    memcpy(keycodes, s_kbd_keycodes, 6);

    if (s_synth_keycode != HID_KEY_NO_PRESS) {
        bool already_present = false;
        for (int i = 0; i < 6; i++) {
            if (keycodes[i] == s_synth_keycode) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            for (int i = 0; i < 6; i++) {
                if (keycodes[i] == HID_KEY_NO_PRESS) {
                    keycodes[i] = s_synth_keycode;
                    break;
                }
            }
        }
    }

    send_keyboard_report(modifiers, keycodes);
}

static void apply_mouse_synth_keys(uint8_t modifiers, uint8_t keycode)
{
    if (modifiers == s_synth_modifiers && keycode == s_synth_keycode) {
        return; // No change - avoid a redundant packet on every mouse report.
    }
    s_synth_modifiers = modifiers;
    s_synth_keycode   = keycode;
    send_merged_keyboard_report();
}

static void handle_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }
    const hid_keyboard_input_report_boot_t *report = (const hid_keyboard_input_report_boot_t *)data;

    uint8_t modifiers = report->modifier.val;
    uint8_t keycodes[6];
    memcpy(keycodes, report->key, sizeof(keycodes));

    if (filter_keyboard_report(&modifiers, keycodes)) {
        s_kbd_modifiers = modifiers;
        memcpy(s_kbd_keycodes, keycodes, 6);
        send_merged_keyboard_report();
    }
}

// Runs a fully-decoded mouse sample (buttons/dx/dy/wheel/pan, regardless
// of whether it came from Report or Boot Protocol) through filter_rules.h
// and sends it on.
static void process_mouse_sample(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    uint8_t synth_modifiers = 0;
    uint8_t synth_keycode = HID_KEY_NO_PRESS;

    if (filter_mouse_report(&buttons, &dx, &dy, &wheel, &pan, &synth_modifiers, &synth_keycode)) {
        send_mouse_report(buttons, dx, dy, wheel, pan);
    }
    apply_mouse_synth_keys(synth_modifiers, synth_keycode);
}

static void handle_mouse_report_boot(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }
    const hid_mouse_input_report_boot_t *report = (const hid_mouse_input_report_boot_t *)data;
    // Boot Protocol mice don't report wheel/pan/buttons 4+ - see
    // mds/2026-08-21_host_report_protocol.md.
    process_mouse_sample(report->buttons.val, report->x_displacement, report->y_displacement, 0, 0);
}

static void handle_mouse_report_generic(const mouse_report_layout_t *layout, const uint8_t *data, size_t length)
{
    uint8_t buttons = 0;
    for (uint8_t i = 0; i < layout->button_count && i < HID_MAX_BUTTONS; i++) {
        if (hid_extract_field(data, length, &layout->buttons[i]) != 0) {
            buttons |= (uint8_t)(1u << i);
        }
    }

    int16_t dx = (int16_t)clamp_i32(hid_extract_field(data, length, &layout->x), INT16_MIN, INT16_MAX);
    int16_t dy = (int16_t)clamp_i32(hid_extract_field(data, length, &layout->y), INT16_MIN, INT16_MAX);
    int8_t wheel = layout->wheel.present
                       ? (int8_t)clamp_i32(hid_extract_field(data, length, &layout->wheel), INT8_MIN, INT8_MAX)
                       : 0;
    int8_t pan = layout->pan.present
                     ? (int8_t)clamp_i32(hid_extract_field(data, length, &layout->pan), INT8_MIN, INT8_MAX)
                     : 0;

    process_mouse_sample(buttons, dx, dy, wheel, pan);
}

// Forwards a keyboard's Consumer Control ("media keys") usage ID as-is,
// on every report - no dedup, matching how the keyboard/mouse paths
// already just forward whatever the physical device sends. Not run
// through filter_rules.h (yet) - nothing has needed to remap/drop a
// media key so far, see mds/2026-08-22_consumer_control.md.
static void handle_consumer_report(const consumer_device_state_t *dev, const uint8_t *data, size_t length)
{
    uint16_t usage_id = (uint16_t)hid_extract_field(data, length, &dev->layout.selector);
    // Debug aid - CONFIG_LOG_MAXIMUM_LEVEL is INFO in this project (see
    // mds/2026-08-22_9buttons_mouse.md), so ESP_LOGD would be compiled out
    // entirely rather than just filtered at runtime; comment this out
    // instead of leaving it live, to avoid spamming every report.
    /*
    ESP_LOGI(TAG, "Consumer report: usage_id=0x%04X (report_len=%d, selector bit_offset=%d bit_length=%d report_id=%d)",
             usage_id, (int)length, dev->layout.selector.bit_offset,
             dev->layout.selector.bit_length, dev->layout.selector.report_id);
    send_consumer_report(usage_id);
    */
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    (void)arg;
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t data_length = 0;
            if (hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length) != ESP_OK) {
                return;
            }
            if (dev_params.proto == HID_PROTOCOL_KEYBOARD &&
                dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                handle_keyboard_report(data, data_length);
            } else if (dev_params.proto == HID_PROTOCOL_MOUSE) {
                mouse_device_state_t *dev = find_mouse_device(hid_device_handle);
                if (dev && dev->use_report_protocol) {
                    handle_mouse_report_generic(&dev->layout, data, data_length);
                } else if (dev && dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                    handle_mouse_report_boot(data, data_length);
                }
            } else {
                consumer_device_state_t *dev = find_consumer_device(hid_device_handle);
                if (dev) {
                    handle_consumer_report(dev, data, data_length);
                }
            }
            // Non-boot-interface keyboards and unparseable non-boot-interface
            // mice are ignored - see mds/2026-08-21_host_report_protocol.md.
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID device disconnected (proto %d)", dev_params.proto);
            unregister_mouse_device(hid_device_handle);
            unregister_consumer_device(hid_device_handle);
            hid_host_device_close(hid_device_handle);
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID device transfer error (proto %d)", dev_params.proto);
            break;
        default:
            break;
    }
}

static void handle_driver_connected(hid_host_device_handle_t hid_device_handle)
{
    hid_host_dev_params_t dev_params;
    if (hid_host_device_get_params(hid_device_handle, &dev_params) != ESP_OK) {
        return;
    }

    bool supports_boot = (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE);
    bool is_mouse      = (dev_params.proto == HID_PROTOCOL_MOUSE);
    bool is_keyboard   = (dev_params.proto == HID_PROTOCOL_KEYBOARD && supports_boot);
    // A HID_PROTOCOL_NONE interface *might* be a keyboard's Consumer
    // Control ("media keys") interface - proto/sub_class can't tell it
    // apart from some other vendor/system-control interface a keyboard
    // exposes, only its Report Descriptor can, and fetching that requires
    // the interface to already be open (HID_INTERFACE_STATE_READY or
    // ACTIVE - see mds/2026-08-22_consumer_control.md), so unlike the
    // fully-unsupported case below this candidate does cost a transient
    // host channel even when it turns out not to be one.
    bool maybe_consumer = (!is_mouse && !is_keyboard && dev_params.proto == HID_PROTOCOL_NONE);

    if (!is_mouse && !is_keyboard && !maybe_consumer) {
        // Don't even open the interface, let alone start it - it claims
        // one of the ESP32-S3's 8 hardware host channels
        // (OTG_NUM_HOST_CHAN, see mds/2026-08-22_multi_device.md), and an
        // interface we're just going to ignore isn't worth spending one
        // on - those are scarce once a hub + a few devices are attached.
        ESP_LOGI(TAG, "HID device connected (unsupported, proto %d) - ignoring, not opened", dev_params.proto);
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback     = hid_host_interface_callback,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(hid_device_handle, &dev_config) != ESP_OK) {
        return;
    }

    if (is_mouse) {
        // Always try Report Protocol first - it's the only way to get
        // wheel/pan/extra buttons, regardless of Boot Interface support
        // (Report Protocol works on any HID mouse; Boot Protocol is only
        // an optional, standardized fallback some mice also support).
        mouse_device_state_t *dev = register_mouse_device(hid_device_handle);
        bool parsed_ok = false;

        size_t desc_len = 0;
        uint8_t *desc = hid_host_get_report_descriptor(hid_device_handle, &desc_len);
        if (dev && desc && desc_len > 0) {
            hid_parse_mouse_report_descriptor(desc, desc_len, &dev->layout);
            parsed_ok = dev->layout.x.present && dev->layout.y.present;
        }

        if (parsed_ok) {
            hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT);
            dev->use_report_protocol = true;
            ESP_LOGI(TAG, "Mouse connected: Report Protocol (buttons=%d wheel=%d pan=%d)",
                     dev->layout.button_count, dev->layout.wheel.present, dev->layout.pan.present);
        } else if (supports_boot) {
            hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
            if (dev) {
                dev->use_report_protocol = false;
            }
            ESP_LOGW(TAG, "Mouse connected: Report Protocol descriptor unparseable, falling back to Boot Protocol (no wheel/pan/extra buttons)");
        } else {
            ESP_LOGE(TAG, "Mouse connected: Report Protocol descriptor unparseable and no Boot Protocol support - ignoring");
        }
    } else if (is_keyboard) {
        // Stays on Boot Protocol: modifiers + 6-key rollover is already
        // everything filter_rules.h can see/remap, and adding a second
        // generic report parser (keyboard usages, not just mouse) isn't
        // needed for what's actually in scope right now.
        hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(hid_device_handle, 0, 0);
        ESP_LOGI(TAG, "Keyboard connected: Boot Protocol");
    } else {
        // maybe_consumer - the only other case that reaches here, see the
        // early return above.
        size_t desc_len = 0;
        uint8_t *desc = hid_host_get_report_descriptor(hid_device_handle, &desc_len);
        consumer_report_layout_t layout = {0};
        if (desc && desc_len > 0) {
            hid_parse_consumer_report_descriptor(desc, desc_len, &layout);
        }
        // Debug aid - see comment on the ESP_LOGI above in
        // handle_consumer_report(); comment this out when not needed.
        /*
        ESP_LOGI(TAG, "proto 0 interface report descriptor (%d bytes):", (int)desc_len);
        ESP_LOG_BUFFER_HEX(TAG, desc, desc_len);
        */

        if (!layout.selector.present) {
            ESP_LOGI(TAG, "HID device connected (proto 0, not a recognized Consumer Control layout) - closing");
            hid_host_device_close(hid_device_handle);
            return;
        }

        consumer_device_state_t *dev = register_consumer_device(hid_device_handle);
        if (!dev) {
            ESP_LOGW(TAG, "Consumer Control device connected but MAX_CONSUMER_DEVICES reached - closing");
            hid_host_device_close(hid_device_handle);
            return;
        }
        dev->layout = layout;
        ESP_LOGI(TAG, "Consumer Control device connected (media keys): bit_offset=%d bit_length=%d report_id=%d",
                 layout.selector.bit_offset, layout.selector.bit_length, layout.selector.report_id);
    }

    hid_host_device_start(hid_device_handle);
}

static void hid_host_driver_event_callback(hid_host_device_handle_t hid_device_handle,
                                           const hid_host_driver_event_t event,
                                           void *arg)
{
    (void)arg;
    if (s_driver_event_queue) {
        xQueueSend(s_driver_event_queue, &hid_device_handle, 0);
    }
    (void)event; // Only HID_HOST_DRIVER_EVENT_CONNECTED exists today.
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_host_app_task(void *arg)
{
    (void)arg;

    hid_host_device_handle_t hid_device_handle;
    while (1) {
        if (xQueueReceive(s_driver_event_queue, &hid_device_handle, portMAX_DELAY)) {
            handle_driver_connected(hid_device_handle);
        }
    }
}

esp_err_t usb_host_task_start(void)
{
    memset(s_kbd_keycodes, HID_KEY_NO_PRESS, sizeof(s_kbd_keycodes));

    if (!resolve_target()) {
        return ESP_FAIL;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        return ESP_FAIL;
    }

    s_driver_event_queue = xQueueCreate(10, sizeof(hid_host_device_handle_t));
    if (!s_driver_event_queue) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t app_task_handle;
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                xTaskGetCurrentTaskHandle(), 2, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority           = 5,
        .stack_size              = 4096,
        .core_id                 = 0,
        .callback                = hid_host_driver_event_callback,
        .callback_arg            = NULL,
    };
    esp_err_t err = hid_host_install(&hid_host_driver_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(usb_host_app_task, "usb_host_app", 4096,
                                NULL, 5, &app_task_handle, 1) != pdTRUE) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Forwarding USB HID input to %s:%d", KVM_TARGET_HOST, UDP_PORT);
    return ESP_OK;
}
