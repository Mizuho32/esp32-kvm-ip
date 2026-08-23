#include "usb_host_max3421.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/usbh.h"
#include "class/hid/hid_host.h"

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
// CFG_TUSB_RHPORT0_MODE in components/tinyusb/host_config/tusb_config.h
// (KVM_ROLE=HOST's own TinyUSB build has no device-mode rhport0 to
// collide with, unlike a board also acting as a USB device on its native
// port - see that file for why). Not tied to any physical pin numbering;
// MAX3421 has no native "port index" of its own since it's just an
// SPI-attached SIE, this is purely a software slot identifier.
#define MAX3421_RHPORT    0

#define MAX3421_SPI_HOST  SPI2_HOST
// MAX3421E datasheet allows up to ~26MHz SPI; start conservative and
// raise later once basic communication is confirmed on real hardware.
#define MAX3421_SPI_CLOCK_HZ (10 * 1000 * 1000)

static spi_device_handle_t s_spi_dev;
static TaskHandle_t s_max3421_task;

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

// ── TinyUSB Host HID callbacks (Phase 1: dump only, see usb_host_max3421.h) ──

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
    tuh_itf_info_t itf_info = {0};
    tuh_hid_itf_get_info(dev_addr, idx, &itf_info);
    ESP_LOGI(TAG, "HID mounted: dev_addr=%d idx=%d class=%d subclass=%d protocol=%d, report descriptor (%d bytes):",
             dev_addr, idx, itf_info.desc.bInterfaceClass, itf_info.desc.bInterfaceSubClass,
             itf_info.desc.bInterfaceProtocol, (int)desc_len);
    ESP_LOG_BUFFER_HEX(TAG, report_desc, desc_len);

    if (!tuh_hid_receive_report(dev_addr, idx)) {
        ESP_LOGW(TAG, "tuh_hid_receive_report() failed right after mount (dev_addr=%d idx=%d)", dev_addr, idx);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    ESP_LOGI(TAG, "HID unmounted: dev_addr=%d idx=%d", dev_addr, idx);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
    ESP_LOGI(TAG, "[%d:%d] raw report (%d bytes):", dev_addr, idx, (int)len);
    ESP_LOG_BUFFER_HEX(TAG, report, len);

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
    // can never fire against a not-yet-assigned s_max3421_task.
    esp_err_t err = max3421_spi_gpio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    err = max3421_spi_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
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
