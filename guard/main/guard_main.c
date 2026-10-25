#include "uart_guard.h"
#include "guard_selftest.h"
#include "guard_board.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/uart_pins.h"

// Both links avoid ROM UART pins and debug/boot pins. UART0 is
// remapped only by the application: the sensor cannot talk to the guard's ROM
// downloader on reset. Leave guard D6/D7 physically unconnected.
static const uart_port_t ports[2] = {UART_NUM_0, UART_NUM_1};
#define NOT_ROM_PIN(pin) ((pin) != U0TXD_GPIO_NUM && (pin) != U0RXD_GPIO_NUM)
_Static_assert(NOT_ROM_PIN(SENSOR_TX) && NOT_ROM_PIN(SENSOR_RX) && NOT_ROM_PIN(FC_TX) && NOT_ROM_PIN(FC_RX),
               "Neither guard link may expose ROM UART pins");
_Static_assert(SENSOR_TX != SENSOR_RX && FC_TX != FC_RX && SENSOR_TX != FC_TX &&
               SENSOR_TX != FC_RX && SENSOR_RX != FC_TX && SENSOR_RX != FC_RX,
               "Guard interfaces require four separate pins");
static const int tx_pins[2] = {SENSOR_TX, FC_TX};
static const int rx_pins[2] = {SENSOR_RX, FC_RX};
static QueueHandle_t events[2];
static uart_guard_t policy;
static uint32_t uart_errors[2];
static char receiver_id[13];
static char session[17], app_sha[65];
static bool selftest_ok;

typedef struct {
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN];
    size_t size, offset;
    uint64_t started_us;
} output_t;
static output_t outputs[2];

static void status(bool ready)
{
    const guard_counters_t *c = &policy.counters;
    char json[1500];
    int size = snprintf(json, sizeof(json),
        "{\"schema\":1,\"kind\":\"status\",\"device_type\":\"uart_guard\","
        "\"firmware_project\":\"droneid_guard\",\"firmware\":\"%s\",\"receiver_id\":\"%s\","
        "\"board_id\":\"" GUARD_BOARD_ID "\",\"chip\":\"" CONFIG_IDF_TARGET "\","
        "\"session\":\"%s\",\"app_sha256\":\"%s\",\"uptime_ms\":%" PRIu64 ","
        "\"policy_selftest\":%s,\"ready\":%s,\"radios_enabled\":false,"
        "\"system_id\":%u,\"component_id\":%u,\"sensor_component\":%u,\"fc_component\":%u,\"target_system\":%u,\"target_component\":%u,"
        "\"sensor\":{\"input_bytes\":%" PRIu32 ",\"dropped_bytes\":%" PRIu32 ",\"rejected\":%" PRIu32
        ",\"framing_errors\":%" PRIu32 ",\"accepted\":%" PRIu32 ",\"replaced\":%" PRIu32
        ",\"expired\":%" PRIu32 ",\"uart_errors\":%" PRIu32 "},"
        "\"fc\":{\"input_bytes\":%" PRIu32 ",\"dropped_bytes\":%" PRIu32 ",\"rejected\":%" PRIu32
        ",\"framing_errors\":%" PRIu32 ",\"output_frames\":%" PRIu32 ",\"output_bytes\":%" PRIu32
        ",\"uart_errors\":%" PRIu32 "},\"encode_errors\":%" PRIu32 "}\n",
        esp_app_get_description()->version, receiver_id, session, app_sha, (uint64_t)esp_timer_get_time()/1000,
        selftest_ok ? "true" : "false", ready ? "true" : "false", policy.config.system_id, policy.config.guard_component,
        policy.config.sensor_component, policy.config.fc_component,
        policy.config.target_system, policy.config.target_component,
        c->input_bytes[0], c->input_dropped[0], c->rejected[0], c->framing_errors[0],
        c->accepted[0], c->replaced[0], c->expired[0], uart_errors[0],
        c->input_bytes[1], c->input_dropped[1], c->rejected[1], c->framing_errors[1],
        c->output_frames[1], c->output_bytes[1], uart_errors[1], c->encode_errors);
    if (size > 0 && (size_t)size < sizeof(json)) usb_serial_jtag_write_bytes(json, size, 0);
}

static bool start_uart(unsigned side)
{
    uart_config_t config = {.baud_rate=115200, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT};
    if (uart_param_config(ports[side], &config) != ESP_OK) return false;
    if (uart_set_pin(ports[side], tx_pins[side], rx_pins[side], UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    gpio_pullup_en(rx_pins[side]);
    return uart_driver_install(ports[side], 512, 0, 16, &events[side], 0) == ESP_OK;
}

static void service_input(unsigned side, uint64_t now)
{
    uart_event_t event;
    bool fault = false;
    for (unsigned i=0; i<16 && xQueueReceive(events[side], &event, 0)==pdTRUE; ++i) {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR || event.type == UART_BREAK) fault = true;
    }
    if (fault) {
        ++uart_errors[side];
        uart_flush_input(ports[side]);
        guard_input_fault(&policy, side);
        return;
    }
    uint8_t bytes[GUARD_INPUT_CHUNK];
    int count = uart_read_bytes(ports[side], bytes, sizeof(bytes), 0);
    if (count < 0) { ++uart_errors[side]; guard_input_fault(&policy, side); }
    else if (count && guard_ingest(&policy, side, bytes, count, now) != (size_t)count) {
        // Drop queued overload instead of retrying bytes or accumulating work.
        uart_flush_input(ports[side]);
    }
}

static void service_output(unsigned side, uint64_t now)
{
    output_t *out = &outputs[side];
    if (!out->size) {
        out->size = guard_poll(&policy, side, now, out->bytes);
        out->offset = 0;
        out->started_us = now;
    }
    if (!out->size) return;
    int sent = uart_tx_chars(ports[side], (const char *)out->bytes+out->offset, out->size-out->offset);
    if (sent > 0) out->offset += (size_t)sent;
    if (out->offset == out->size) out->size = 0;
    else if (sent < 0 || now-out->started_us >= 250000) {
        ++uart_errors[side];
        out->size = 0; // No retries or unbounded pending output.
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size=256, .tx_buffer_size=2048};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    uint8_t mac[6];
    // The generic efuse helper writes an EUI-64 on C5/C6. Request the
    // six-byte factory MAC explicitly: it also matches native USB identity.
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY));
    snprintf(receiver_id, sizeof(receiver_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(session, sizeof(session), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    const esp_app_desc_t *app = esp_app_get_description();
    for (unsigned i=0; i<32; ++i) snprintf(app_sha+2*i, 3, "%02x", app->app_elf_sha256[i]);
    guard_config_t config = {.system_id=CONFIG_GUARD_SYSTEM_ID, .sensor_component=CONFIG_GUARD_SENSOR_COMPONENT,
        .guard_component=CONFIG_GUARD_COMPONENT, .fc_component=CONFIG_GUARD_FC_COMPONENT,
        .target_system=CONFIG_GUARD_TARGET_SYSTEM, .target_component=CONFIG_GUARD_TARGET_COMPONENT};
    selftest_ok = guard_selftest();
    bool ready = guard_init(&policy, &config, esp_timer_get_time()) && selftest_ok;
    if (ready) ready = start_uart(GUARD_SENSOR) && start_uart(GUARD_FC);
    if (!ready) {
        // No worker starts, and neither UART is ever passed application output.
        gpio_reset_pin(tx_pins[GUARD_FC]);
        gpio_set_direction(tx_pins[GUARD_FC], GPIO_MODE_INPUT);
    }
    uint64_t next_status = 0;
    for (;;) {
        uint64_t now = esp_timer_get_time();
        if (ready) {
            service_input(GUARD_SENSOR, now);
            service_input(GUARD_FC, now);
            service_output(GUARD_FC, now);
            service_output(GUARD_SENSOR, now);
        }
        // USB is diagnostics only. No runtime configuration, flash trigger,
        // passthrough, network stack, or command interpreter is present.
        uint8_t ignored[64];
        usb_serial_jtag_read_bytes(ignored, sizeof(ignored), 0);
        if (now >= next_status) { status(ready); next_status=now+2000000; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
