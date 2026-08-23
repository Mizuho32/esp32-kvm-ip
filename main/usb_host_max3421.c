#include "usb_host_max3421.h"

#include <string.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/usbh.h"
#include "class/hid/hid_host.h"

#include "hid_forwarder.h"
#include "hid_report_parser.h"

#define TAG "USBHOST_MAX3421"

// Actual wiring (mds/2026-08-23_filter_conv_router_with_max3421.md) -
// MOSI/MISO/SCLK are the ESP32-S3-Plus board's standard SPI pins. These
// are plain GPIOs, independent of the native OTG Host path's fixed
// USB_DP/USB_DM silicon pins (usb_host_task.c) and of the UART0 console
// (GPIO43/44), so there's no conflict with either.
#define MAX3421_PIN_MOSI  9
#define MAX3421_PIN_MISO  8
#define MAX3421_PIN_SCLK  7
#define MAX3421_PIN_CS    4
#define MAX3421_PIN_RST   5
#define MAX3421_PIN_INT   6

// Logical TinyUSB root-hub port number for the MAX3421E - see
// CFG_TUSB_RHPORT1_MODE in components/tinyusb/host_config/tusb_config.h.
// rhport0 is native OTG as a Device (main/usb_device_typec.c, Phase2
// type-c output) - MAX3421E gets rhport1 instead so the two don't
// collide. Not tied to any physical pin numbering; MAX3421 has no native
// "port index" of its own since it's just an SPI-attached SIE, this is
// purely a software slot identifier.
#define MAX3421_RHPORT    1

#define MAX3421_SPI_HOST  SPI2_HOST
// MAX3421E datasheet allows up to ~26MHz SPI, but that's optimistic over
// breadboard jumper wires - 10MHz was unreliable (devices mounting then
// unmounting almost immediately). Empirically, 5MHz turned out more
// stable than 1MHz on this wiring (not obviously lower is better) - see
// mds/2026-08-23_filter_conv_router_with_max3421.md.
#define MAX3421_SPI_CLOCK_HZ (5 * 1000 * 1000)

static spi_device_handle_t s_spi_dev;
static TaskHandle_t s_max3421_task;
static bool s_bus_initialized;

// ── Board API required by components/tinyusb's core (tusb_common.h) ───
// A millisecond tick source - TinyUSB has no built-in notion of time
// without an app-supplied one.

uint32_t tusb_time_millis_api(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void tusb_time_delay_ms_api(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// ── Board API required by components/tinyusb's hcd_max3421.c ──────────
// (extern declarations in portable/analog/max3421/hcd_max3421.h - these
// three functions are the entire hardware-specific surface the driver
// needs from us.)

void tuh_max3421_int_api(uint8_t rhport, bool enabled)
{
    (void)rhport;
    if (enabled) {
        gpio_intr_enable(MAX3421_PIN_INT);
    } else {
        gpio_intr_disable(MAX3421_PIN_INT);
    }
}

void tuh_max3421_spi_cs_api(uint8_t rhport, bool active)
{
    (void)rhport;
    // Active-low CS, and deliberately NOT the SPI driver's own automatic
    // per-transaction CS (spics_io_num = -1 below) - hcd_max3421.c needs
    // to hold CS asserted across several back-to-back xfer_api calls
    // (e.g. one for the FIFO register address, another for the FIFO
    // payload itself), which per-transaction auto-CS can't express.
    gpio_set_level(MAX3421_PIN_CS, active ? 0 : 1);
}

bool tuh_max3421_spi_xfer_api(uint8_t rhport, uint8_t const *tx_buf, uint8_t *rx_buf, size_t xfer_bytes)
{
    (void)rhport;
    if (xfer_bytes == 0) {
        return true;
    }
    spi_transaction_t t = {
        .length    = xfer_bytes * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    return spi_device_polling_transmit(s_spi_dev, &t) == ESP_OK;
}

// ── GPIO interrupt → wake the host task ────────────────────────────────
// Deliberately NOT calling hcd_int_handler()/SPI directly from ISR
// context - keeps every SPI transaction on one task, avoiding the extra
// care needed to make tuh_max3421_spi_xfer_api() itself ISR-safe. Costs a
// bit of interrupt-to-service latency (bounded by the task wake time
// below), acceptable for HID input rates.
static void IRAM_ATTR max3421_gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_max3421_task, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t max3421_spi_gpio_init(void)
{
    gpio_config_t cs_conf = {
        .pin_bit_mask = 1ULL << MAX3421_PIN_CS,
        .mode         = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&cs_conf);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(MAX3421_PIN_CS, 1); // deasserted (active-low)

    // RESET is active-low - hold it low briefly, then release, before any
    // SPI traffic (MAX3421E datasheet: registers aren't valid until after
    // reset is released and the oscillator has stabilized).
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << MAX3421_PIN_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    err = gpio_config(&rst_conf);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(MAX3421_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(MAX3421_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10)); // oscillator settling time

    gpio_config_t int_conf = {
        .pin_bit_mask = 1ULL << MAX3421_PIN_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE, // MAX3421E INT is open-drain, active-low
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&int_conf);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE = already installed elsewhere
        return err;
    }
    return gpio_isr_handler_add(MAX3421_PIN_INT, max3421_gpio_isr, NULL);
}

static esp_err_t max3421_spi_bus_init(void)
{
    spi_bus_config_t bus_conf = {
        .mosi_io_num = MAX3421_PIN_MOSI,
        .miso_io_num = MAX3421_PIN_MISO,
        .sclk_io_num = MAX3421_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(MAX3421_SPI_HOST, &bus_conf, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev_conf = {
        .mode           = 0, // MAX3421E: CPOL=0, CPHA=0
        .clock_speed_hz = MAX3421_SPI_CLOCK_HZ,
        .spics_io_num   = -1, // manual CS - see tuh_max3421_spi_cs_api() above
        .queue_size     = 1,
    };
    return spi_bus_add_device(MAX3421_SPI_HOST, &dev_conf, &s_spi_dev);
}

// Shared by usb_host_max3421_probe() and max3421_host_task() - the probe
// runs first (from app_main(), before deciding which Host backend to
// start at all) and the task reuses the same already-initialized
// GPIO/SPI state instead of re-running spi_bus_initialize(), which would
// fail the second time (ESP_ERR_INVALID_STATE, bus already claimed).
static esp_err_t max3421_ensure_bus_initialized(void)
{
    if (s_bus_initialized) {
        return ESP_OK;
    }
    esp_err_t err = max3421_spi_gpio_init();
    if (err != ESP_OK) {
        return err;
    }
    err = max3421_spi_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    s_bus_initialized = true;
    return ESP_OK;
}

// Same register address hcd_max3421.c's own internal revision check uses
// (see REVISION_ADDR in components/tinyusb/src/portable/analog/max3421/
// hcd_max3421.c) - not exported via hcd_max3421.h, so duplicated here.
// Read directly via tuh_max3421_spi_cs_api()/spi_xfer_api() rather than
// the driver's own tuh_max3421_reg_read(): that helper locks a mutex
// (_hcd_data.spi_mutex) that's only created inside hcd_init(), which
// hasn't run yet at probe time.
#define MAX3421_REVISION_ADDR (18u << 3)

bool usb_host_max3421_probe(void)
{
    esp_err_t err = max3421_ensure_bus_initialized();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "MAX3421 probe: GPIO/SPI init failed (%s) - assuming not present", esp_err_to_name(err));
        return false;
    }

    uint8_t tx_buf[2] = { MAX3421_REVISION_ADDR, 0 };
    uint8_t rx_buf[2] = { 0, 0 };
    tuh_max3421_spi_cs_api(MAX3421_RHPORT, true);
    bool xfer_ok = tuh_max3421_spi_xfer_api(MAX3421_RHPORT, tx_buf, rx_buf, 2);
    tuh_max3421_spi_cs_api(MAX3421_RHPORT, false);

    // v1 is 0x01, v2 is 0x12, v3 is 0x13 (same check hcd_init() itself
    // makes). Not bulletproof against a floating/garbage bus coincidentally
    // matching one of these bytes, but no worse than what the driver
    // already relies on internally - see
    // mds/2026-08-23_filter_conv_router_with_max3421.md for the earlier
    // "looked alive even seemingly unpowered" report on this wiring.
    uint8_t revision = rx_buf[1];
    bool present = xfer_ok && (revision == 0x01 || revision == 0x12 || revision == 0x13);
    ESP_LOGI(TAG, "MAX3421 probe: xfer_ok=%d revision=0x%02x -> %s",
             xfer_ok, revision, present ? "present" : "not present");
    return present;
}

// ── Per-device state, purely for dispatch/parsing - see
// mds/2026-08-21_host_report_protocol.md / mds/2026-08-22_9buttons_mouse.md
// for what these fields are for. Keyed by (dev_addr, idx) instead of
// hid_host_device_handle_t (usb_host_task.c's native OTG equivalent).
// Deliberately NOT touching SET_PROTOCOL/protocol negotiation anywhere in
// this file - see mds/2026-08-23_filter_conv_router_with_max3421.md:
// this used to call tuh_hid_set_protocol() per device on top of the
// automatic enum-time one (tuh_hid_set_default_protocol() below +
// CFG_TUH_HID_SET_PROTOCOL_ON_ENUM, default on), and that extra explicit
// call - even after fixing it to wait for its completion callback before
// starting tuh_hid_receive_report() - kept reproducing occasional
// wrong-length reports on this specific wireless dongle. Going back to
// exactly what the earlier "dump everything" smoke test did (only the
// automatic enum-time SET_PROTOCOL, driven by the global default below,
// nothing per-device) is what's actually been confirmed reliable on real
// hardware, so this only adds dispatch/parsing on top of that, without
// touching protocol negotiation at all. ──

#define MAX_MOUSE_DEVICES 4
typedef struct {
    uint8_t                  dev_addr;
    uint8_t                  idx;
    bool                     use_report_protocol;
    mouse_report_layout_t    layout;
    consumer_report_layout_t consumer_layout;
} max3421_mouse_state_t;
static max3421_mouse_state_t s_mouse_devices[MAX_MOUSE_DEVICES];
static int s_mouse_device_count;

#define MAX_CONSUMER_DEVICES 4
typedef struct {
    uint8_t                  dev_addr;
    uint8_t                  idx;
    consumer_report_layout_t layout;
} max3421_consumer_state_t;
static max3421_consumer_state_t s_consumer_devices[MAX_CONSUMER_DEVICES];
static int s_consumer_device_count;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static max3421_mouse_state_t *find_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].dev_addr == dev_addr && s_mouse_devices[i].idx == idx) {
            return &s_mouse_devices[i];
        }
    }
    return NULL;
}

static max3421_mouse_state_t *register_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    if (s_mouse_device_count >= MAX_MOUSE_DEVICES) {
        return NULL;
    }
    max3421_mouse_state_t *d = &s_mouse_devices[s_mouse_device_count++];
    memset(d, 0, sizeof(*d));
    d->dev_addr = dev_addr;
    d->idx      = idx;
    return d;
}

static void unregister_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].dev_addr == dev_addr && s_mouse_devices[i].idx == idx) {
            s_mouse_devices[i] = s_mouse_devices[--s_mouse_device_count];
            return;
        }
    }
}

static max3421_consumer_state_t *find_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].dev_addr == dev_addr && s_consumer_devices[i].idx == idx) {
            return &s_consumer_devices[i];
        }
    }
    return NULL;
}

static max3421_consumer_state_t *register_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    if (s_consumer_device_count >= MAX_CONSUMER_DEVICES) {
        return NULL;
    }
    max3421_consumer_state_t *d = &s_consumer_devices[s_consumer_device_count++];
    memset(d, 0, sizeof(*d));
    d->dev_addr = dev_addr;
    d->idx      = idx;
    return d;
}

static void unregister_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].dev_addr == dev_addr && s_consumer_devices[i].idx == idx) {
            s_consumer_devices[i] = s_consumer_devices[--s_consumer_device_count];
            return;
        }
    }
}

static void handle_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_report_t)) {
        return;
    }
    const hid_keyboard_report_t *report = (const hid_keyboard_report_t *)data;
    hid_forwarder_keyboard_report(report->modifier, report->keycode);
}

static void handle_mouse_report_boot(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_mouse_report_t)) {
        return;
    }
    const hid_mouse_report_t *report = (const hid_mouse_report_t *)data;
    // Boot Protocol mice don't report wheel/pan/buttons 4+ - see
    // mds/2026-08-21_host_report_protocol.md.
    hid_forwarder_mouse_sample(report->buttons, report->x, report->y, 0, 0);
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

    hid_forwarder_mouse_sample(buttons, dx, dy, wheel, pan);
}

static void handle_consumer_report(const consumer_report_layout_t *layout, const uint8_t *data, size_t length)
{
    uint16_t usage_id = (uint16_t)hid_extract_field(data, length, &layout->selector);
    hid_forwarder_consumer(usage_id);
}

// ── TinyUSB Host HID callbacks ──────────────────────────────────────────
// mount/report_received still do exactly what the dump-only smoke test
// did as far as TinyUSB API calls go (just tuh_hid_receive_report() at
// the end, no protocol negotiation) - only the descriptor
// parsing/dispatch bookkeeping is new.

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
    tuh_itf_info_t itf_info = {0};
    tuh_hid_itf_get_info(dev_addr, idx, &itf_info);
    uint8_t proto = itf_info.desc.bInterfaceProtocol;
    ESP_LOGI(TAG, "HID mounted: dev_addr=%d idx=%d class=%d subclass=%d protocol=%d, report descriptor (%d bytes):",
             dev_addr, idx, itf_info.desc.bInterfaceClass, itf_info.desc.bInterfaceSubClass,
             proto, (int)desc_len);
    ESP_LOG_BUFFER_HEX(TAG, report_desc, desc_len);

    if (proto == HID_ITF_PROTOCOL_MOUSE) {
        max3421_mouse_state_t *dev = register_mouse_device(dev_addr, idx);
        if (dev && report_desc && desc_len > 0) {
            hid_parse_mouse_report_descriptor(report_desc, desc_len, &dev->layout);
            dev->use_report_protocol = dev->layout.x.present && dev->layout.y.present;

            // Some mice bundle a Consumer Control selector (volume,
            // forward/back, ...) into this same interface on a separate
            // Report ID - see mds/2026-08-22_9buttons_mouse.md.
            hid_parse_consumer_report_descriptor(report_desc, desc_len, &dev->consumer_layout);
            if (dev->consumer_layout.selector.present) {
                ESP_LOGI(TAG, "Mouse also has a bundled Consumer Control selector (report_id=%d bit_length=%d)",
                         dev->consumer_layout.selector.report_id, dev->consumer_layout.selector.bit_length);
            }
            ESP_LOGI(TAG, "Mouse connected (use_report_protocol=%d buttons=%d wheel=%d pan=%d)",
                     dev->use_report_protocol, dev->layout.button_count, dev->layout.wheel.present, dev->layout.pan.present);
        }
    } else if (proto == HID_ITF_PROTOCOL_NONE) {
        // Might be a keyboard's Consumer Control ("media keys") interface,
        // or a standalone Consumer Control device - see
        // mds/2026-08-22_consumer_control.md.
        consumer_report_layout_t layout = {0};
        if (report_desc && desc_len > 0) {
            hid_parse_consumer_report_descriptor(report_desc, desc_len, &layout);
        }
        if (layout.selector.present) {
            max3421_consumer_state_t *dev = register_consumer_device(dev_addr, idx);
            if (dev) {
                dev->layout = layout;
                ESP_LOGI(TAG, "Consumer Control device connected (media keys): bit_offset=%d bit_length=%d report_id=%d",
                         layout.selector.bit_offset, layout.selector.bit_length, layout.selector.report_id);
            }
        }
    }
    // proto == HID_ITF_PROTOCOL_KEYBOARD needs no registration - decoded
    // directly as a fixed-layout Boot report in tuh_hid_report_received_cb().

    if (!tuh_hid_receive_report(dev_addr, idx)) {
        ESP_LOGW(TAG, "tuh_hid_receive_report() failed right after mount (dev_addr=%d idx=%d)", dev_addr, idx);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    ESP_LOGI(TAG, "HID unmounted: dev_addr=%d idx=%d", dev_addr, idx);
    unregister_mouse_device(dev_addr, idx);
    unregister_consumer_device(dev_addr, idx);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
    // Debug aid - see usb_host_task.c's equivalent toggles. Temporarily
    // ON while diagnosing report-length issues - comment back out once
    // confirmed stable.
    //*
    ESP_LOGI(TAG, "[%d:%d] raw report (%d bytes):", dev_addr, idx, (int)len);
    ESP_LOG_BUFFER_HEX(TAG, report, len);
    //*/

    tuh_itf_info_t itf_info = {0};
    tuh_hid_itf_get_info(dev_addr, idx, &itf_info);
    uint8_t proto = itf_info.desc.bInterfaceProtocol;

    if (proto == HID_ITF_PROTOCOL_KEYBOARD) {
        handle_keyboard_report(report, len);
    } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
        max3421_mouse_state_t *dev = find_mouse_device(dev_addr, idx);
        if (dev && dev->consumer_layout.selector.present &&
            dev->consumer_layout.selector.report_id != 0 &&
            len >= 1 && report[0] == dev->consumer_layout.selector.report_id) {
            handle_consumer_report(&dev->consumer_layout, report, len);
        } else if (dev && dev->use_report_protocol) {
            handle_mouse_report_generic(&dev->layout, report, len);
        } else if (dev) {
            handle_mouse_report_boot(report, len);
        }
    } else {
        max3421_consumer_state_t *dev = find_consumer_device(dev_addr, idx);
        if (dev) {
            handle_consumer_report(&dev->layout, report, len);
        }
    }

    // Keep the report stream going - tuh_hid does not auto-repeat this
    // like the native usb_host_hid driver does.
    if (!tuh_hid_receive_report(dev_addr, idx)) {
        ESP_LOGW(TAG, "tuh_hid_receive_report() failed (dev_addr=%d idx=%d)", dev_addr, idx);
    }
}

static void max3421_host_task(void *arg)
{
    (void)arg;

    // Done here, after the task handle itself is already running (rather
    // than by the caller before xTaskCreate()), so max3421_gpio_isr()
    // can never fire against a not-yet-assigned s_max3421_task. Usually
    // already done by usb_host_max3421_probe() by this point (see
    // main_host.c) - max3421_ensure_bus_initialized() is a no-op if so.
    esp_err_t err = max3421_ensure_bus_initialized();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO/SPI init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    tuh_configure_param_t cfg = {
        .max3421 = {
            .max_nak = 1, // favor lower latency over CPU/SPI bus usage
            .cpuctl  = 0,
            .pinctl  = 0,
        },
    };
    tuh_configure(MAX3421_RHPORT, TUH_CFGID_MAX3421, &cfg);

    // TinyUSB defaults to Boot Protocol (hid_host.c's _hidh_default_protocol),
    // same as the RP2040 cross-test needed to override - see
    // mds/2026-08-22_rp2040_host_check.md. Without this, mice come back as
    // plain 3-byte buttons/dx/dy, not the Report ID-tagged Report Protocol
    // data (wheel, extra buttons) this project actually wants. This is the
    // ONLY protocol negotiation this file does - see the block comment
    // above the device-state tables for why nothing per-device is added
    // on top of it.
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

    const tusb_rhport_init_t rh_init = {
        .role  = TUSB_ROLE_HOST,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tuh_rhport_init(MAX3421_RHPORT, &rh_init)) {
        ESP_LOGE(TAG, "tuh_init() failed - MAX3421E not responding? (check wiring/pins in usb_host_max3421.c)");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TinyUSB Host (MAX3421E) initialized on rhport %d - waiting for devices...", MAX3421_RHPORT);

    while (1) {
        // Woken early by the GPIO ISR (max3421_gpio_isr) on a real
        // interrupt; the timeout is just a safety net against a missed
        // edge, not the primary mechanism.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        hcd_int_handler(MAX3421_RHPORT, false);
        tuh_task();
    }
}

esp_err_t usb_host_max3421_task_start(void)
{
    if (xTaskCreate(max3421_host_task, "usb_host_max3421", 4096, NULL, 5, &s_max3421_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
