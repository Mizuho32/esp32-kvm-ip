#include "uart_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mruby_filter.h" // mruby_dispatch_uart_rx()

#define TAG "UARTBRIDGE"

// ESP32-S3 has 3 UART controllers total (UART_NUM_0/1/2) - a tiny fixed
// table keyed by port number is simpler than a dynamic list, and every
// caller here already deals in small integer port numbers (mruby_filter.c's
// `port:` DSL option), not uart_port_t handles.
#define UART_BRIDGE_MAX_PORTS 3

// One esp_read_bytes() call's max chunk size, and therefore also the
// largest single payload a `to`/`branch` stage ever sees for a `:uart`-
// kind pipeline event - comfortably under RAW_BYTES_MAX_LEN (protocol.h),
// which a :udp sink downstream of a :uart source needs to fit each chunk
// into unfragmented. Real UART traffic (log lines, AT command responses)
// arrives in bursts well under this in practice; a burst larger than this
// spans multiple dispatch calls/pipeline events rather than one.
#define UART_BRIDGE_CHUNK_MAX      256
#define UART_BRIDGE_RX_BUF_SIZE    1024
#define UART_BRIDGE_READ_TIMEOUT_MS 20

typedef struct {
    bool configured;
    bool rx_started;
    int  rx_pin; // UART_BRIDGE_PIN_UNUSED if this port was never given one
    int  tx_pin; // UART_BRIDGE_PIN_UNUSED if this port was never given one
    int  baud;
} uart_bridge_port_t;

static uart_bridge_port_t s_ports[UART_BRIDGE_MAX_PORTS];

static bool port_valid(int port)
{
    return port >= 0 && port < UART_BRIDGE_MAX_PORTS;
}

esp_err_t uart_bridge_configure(int port, int rx_pin, int tx_pin, int baud)
{
    if (!port_valid(port)) {
        ESP_LOGE(TAG, "port %d out of range (0..%d)", port, UART_BRIDGE_MAX_PORTS - 1);
        return ESP_ERR_INVALID_ARG;
    }
    uart_bridge_port_t *p = &s_ports[port];

    if (p->configured && p->baud != baud) {
        ESP_LOGE(TAG, "port %d: baud mismatch between source/sink declarations (already %d, now %d) - keeping %d, ignoring this one",
                 port, p->baud, baud, p->baud);
        return ESP_ERR_INVALID_STATE;
    }

    if (!p->configured) {
        uart_config_t cfg = {
            .baud_rate = baud,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_driver_install(port, UART_BRIDGE_RX_BUF_SIZE, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: uart_driver_install failed: %s", port, esp_err_to_name(err));
            return err;
        }
        err = uart_param_config(port, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: uart_param_config failed: %s", port, esp_err_to_name(err));
            return err;
        }
        p->configured = true;
        p->baud        = baud;
        p->rx_pin       = UART_BRIDGE_PIN_UNUSED;
        p->tx_pin       = UART_BRIDGE_PIN_UNUSED;
    }

    // rx_pin/tx_pin accumulate across separate source/sink calls for the
    // same port - uart_set_pin() always takes both, so re-supply
    // whichever side an earlier call already set.
    if (rx_pin != UART_BRIDGE_PIN_UNUSED) p->rx_pin = rx_pin;
    if (tx_pin != UART_BRIDGE_PIN_UNUSED) p->tx_pin = tx_pin;

    int wire_tx = (p->tx_pin == UART_BRIDGE_PIN_UNUSED) ? UART_PIN_NO_CHANGE : p->tx_pin;
    int wire_rx = (p->rx_pin == UART_BRIDGE_PIN_UNUSED) ? UART_PIN_NO_CHANGE : p->rx_pin;
    esp_err_t err = uart_set_pin(port, wire_tx, wire_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: uart_set_pin(tx=%d, rx=%d) failed: %s", port, wire_tx, wire_rx, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "port %d: configured (rx=%d tx=%d baud=%d)", port, p->rx_pin, p->tx_pin, p->baud);
    return ESP_OK;
}

static void uart_bridge_rx_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    uint8_t buf[UART_BRIDGE_CHUNK_MAX];
    while (1) {
        int len = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(UART_BRIDGE_READ_TIMEOUT_MS));
        if (len > 0) {
            mruby_dispatch_uart_rx(port, buf, (size_t)len);
        }
        // len == 0 (nothing arrived within the timeout) or < 0 (driver
        // error) both just loop back and try again - this task has
        // nothing else to do between chunks.
    }
}

void uart_bridge_start_rx(int port)
{
    if (!port_valid(port)) {
        return;
    }
    uart_bridge_port_t *p = &s_ports[port];
    if (!p->configured || p->rx_pin == UART_BRIDGE_PIN_UNUSED || p->rx_started) {
        return;
    }
    p->rx_started = true;
    char name[16];
    snprintf(name, sizeof(name), "uart_bridge%d", port);
    if (xTaskCreate(uart_bridge_rx_task, name, 3072, (void *)(intptr_t)port, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "port %d: failed to start RX task", port);
        p->rx_started = false;
    }
}

void uart_bridge_write(int port, const uint8_t *data, size_t len)
{
    if (!port_valid(port)) {
        return;
    }
    uart_bridge_port_t *p = &s_ports[port];
    if (!p->configured || p->tx_pin == UART_BRIDGE_PIN_UNUSED) {
        ESP_LOGW(TAG, "port %d: write requested but no tx_pin configured (no `sink ..., :uart, tx: ...` for this port?) - dropping %u bytes",
                 port, (unsigned)len);
        return;
    }
    uart_write_bytes(port, (const char *)data, len);
}
