#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bench.h"
#include "board.h"
#include "rid.h"
#include "fc_uart.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C5)
#error "This board configuration requires the esp32c5 target."
#endif

static QueueHandle_t observations;
static bench_radio_stats_t radio_stats;
static bench_role_t role;
static bench_tx_transport_t tx_transport;
static rid_identity_t identity;
typedef struct {
    uint8_t version;
    uint8_t role;
    uint8_t transport;
    rid_identity_t identity;
} saved_settings_t;
static char receiver_id[13];
static char session[17];
static _Atomic uint32_t usb_drops;
static fc_config_t fc_config;
static nvs_handle_t config;
static int64_t identify_until; // Owned by the USB task; never persisted.

static const char *role_name(bench_role_t value)
{
    return value == BENCH_TX ? "tx" : value == BENCH_BOTH ? "both" : "rx";
}

// Only the USB task writes this stream. The radio task never waits for USB.
static bool send_record(const char *record, size_t len)
{
    if (!usb_serial_jtag_is_connected() ||
        usb_serial_jtag_write_bytes(record, len, pdMS_TO_TICKS(10)) != (int)len) {
        ++usb_drops;
        return false;
    }
    return true;
}

static void status(const char *request_id)
{
    char line[3000], uart_json[1100];
    if (!fc_uart_json(uart_json,sizeof(uart_json),(uint64_t)esp_timer_get_time())) strcpy(uart_json,"null");
    const char *phases[] = {"idle", "ble", "wifi_6", "wifi_149"};
    unsigned phase = atomic_load(&radio_stats.listen_phase);
    char hello_hex[RID_HELLO_MAX * 2 + 1] = "";
    for (size_t i = 0; identity.hello[i]; ++i)
        snprintf(hello_hex + 2 * i, 3, "%02x", (unsigned char)identity.hello[i]);
    int n = snprintf(line, sizeof(line),
        "{\"schema\":1,\"kind\":\"status\",\"receiver_id\":\"%s\",\"session\":\"%s\","
        "\"request_id\":\"%s\",\"role\":\"%s\",\"firmware\":\"0.5.0\","
        "\"identify_supported\":true,\"configuration_version\":1,\"broadcast_id\":\"%s\",\"hello_hex\":\"%s\","
        "\"tx_transport\":\"%s\",\"rx_transports\":%s,\"rx_schedule\":\"%s\","
        "\"rx_phase\":\"%s\",\"radio_ready\":%s,\"bench_transmitter\":%s,"
        "\"uptime_us\":%" PRId64 ",\"free_heap_bytes\":%" PRIu32 ","
        "\"advertisements_seen\":%" PRIu32 ",\"rid_observations\":%" PRIu32 ","
        "\"malformed_advertisements\":%" PRIu32 ",\"rx_queue_dropped\":%" PRIu32 ","
        "\"usb_records_dropped\":%" PRIu32 ",\"tx_advertisement_sets\":%" PRIu32 ","
        "\"radio_errors\":%" PRIu32 ",\"ble_observations\":%" PRIu32 ","
        "\"wifi_observations\":%" PRIu32 ",\"wifi_frames_seen\":%" PRIu32 ","
        "\"wifi_tx_frames\":%" PRIu32 ",\"scan_cycles\":%" PRIu32 ","
        "\"ble_listen_ms\":%" PRIu32 ",\"wifi_6_listen_ms\":%" PRIu32 ","
        "\"wifi_149_listen_ms\":%" PRIu32 ",\"uart\":%s}\n",
        receiver_id, session, request_id, role_name(role),
        identity.id, hello_hex,
        tx_transport == BENCH_TX_WIFI ? "wifi_beacon" : "ble_legacy",
        role == BENCH_TX ? "[]" : "[\"ble_legacy\",\"wifi_beacon\",\"wifi_nan\"]",
        role == BENCH_TX ? "disabled" : "time_sliced",
        phases[phase <= BENCH_LISTEN_WIFI149 ? phase : 0],
        atomic_load(&radio_stats.ready) ? "true" : "false",
        role != BENCH_RX ? "true" : "false", esp_timer_get_time(), esp_get_free_heap_size(),
        atomic_load(&radio_stats.advertisements), atomic_load(&radio_stats.observations),
        atomic_load(&radio_stats.malformed), atomic_load(&radio_stats.queue_drops),
        atomic_load(&usb_drops), atomic_load(&radio_stats.tx_sets), atomic_load(&radio_stats.radio_errors),
        atomic_load(&radio_stats.ble_observations), atomic_load(&radio_stats.wifi_observations),
        atomic_load(&radio_stats.wifi_frames), atomic_load(&radio_stats.wifi_tx_frames),
        atomic_load(&radio_stats.scan_cycles), atomic_load(&radio_stats.listen_ms[BENCH_LISTEN_BLE]),
        atomic_load(&radio_stats.listen_ms[BENCH_LISTEN_WIFI6]),
        atomic_load(&radio_stats.listen_ms[BENCH_LISTEN_WIFI149]),uart_json);
    if (n > 0 && (size_t)n < sizeof(line)) send_record(line, (size_t)n);
}

static void reply(const char *request_id, bool ok, const char *message)
{
    // Fixed strings and validated tokens only, never unchecked command bytes.
    char line[300];
    int n = snprintf(line, sizeof(line),
        "{\"schema\":1,\"kind\":\"reply\",\"receiver_id\":\"%s\",\"session\":\"%s\","
        "\"request_id\":\"%s\",\"ok\":%s,\"message\":\"%s\"}\n",
        receiver_id, session, request_id, ok ? "true" : "false", message);
    if (n > 0 && (size_t)n < sizeof(line)) send_record(line, (size_t)n);
}

static bool valid_token(const char *token)
{
    for (size_t i = 0; token[i]; ++i) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static void save_settings(uint8_t selected_role, uint8_t selected_transport,
                           const rid_identity_t *selected_identity, const char *token)
{
    const saved_settings_t value = { .version = 1, .role = selected_role,
        .transport = selected_transport, .identity = *selected_identity };
    // One NVS blob: all four fields change together, including across power loss.
    esp_err_t rc = nvs_set_blob(config, "settings_v1", &value, sizeof(value));
    if (rc == ESP_OK) rc = nvs_commit(config);
    if (rc != ESP_OK) { reply(token, false, "failed to persist configuration"); return; }
    reply(token, true, "configuration saved; restarting");
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

static void configure(const char *line)
{
    char r[6], t[6], id_hex[39], hello_hex[47], token[20], extra[2];
    int count = sscanf(line, "configure %5s %5s %38s %46s %19s %1s",
                       r, t, id_hex, hello_hex, token, extra);
    if (count != 5 || !valid_token(token)) { reply("", false, "invalid configure command"); return; }
    int selected_role = !strcmp(r, "rx") ? BENCH_RX : !strcmp(r, "tx") ? BENCH_TX :
                        !strcmp(r, "both") ? BENCH_BOTH : -1;
    int selected_transport = !strcmp(t, "wifi") ? BENCH_TX_WIFI : !strcmp(t, "ble") ? BENCH_TX_BLE : -1;
    rid_identity_t selected;
    if (selected_role < 0 || selected_transport < 0 ||
        !rid_identity_from_hex(&selected, id_hex, hello_hex)) {
        reply(token, false, "invalid role, transport, ID, or hello message"); return;
    }
    save_settings(selected_role, selected_transport, &selected, token);
}

static void command(const char *line)
{
    if (!strncmp(line,"uart ",5)) {
        // Split off the request token before strict numeric configuration parsing.
        char values[160], token[20];
        const char *last=strrchr(line+5,' ');
        if (!last || (size_t)(last-(line+5))>=sizeof(values) || strlen(last+1)>=sizeof(token)) {
            reply("",false,"invalid uart command"); return;
        }
        memcpy(values,line+5,(size_t)(last-(line+5))); values[last-(line+5)]=0;
        strcpy(token,last+1);
        fc_config_t selected;
        if (!*token || !valid_token(token) || !fc_config_parse(&selected,values)) {
            reply(valid_token(token)?token:"",false,"invalid UART configuration"); return;
        }
        esp_err_t rc=nvs_set_blob(config,"uart_v1",&selected,sizeof(selected));
        if (rc==ESP_OK) rc=nvs_commit(config);
        if (rc!=ESP_OK) {reply(token,false,"failed to persist UART configuration"); return;}
        reply(token,true,"UART configuration saved; restarting");
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
        vTaskDelay(pdMS_TO_TICKS(150)); esp_restart();
    }
    if (!strncmp(line, "configure ", 10)) { configure(line); return; }
    char verb[16] = "", arg[20] = "", token[20] = "", extra[2];
    int count = sscanf(line, "%15s %19s %19s %1s", verb, arg, token, extra);
    if (count < 1) return;
    if (!strcmp(verb, "status") && count <= 2 && valid_token(arg)) {
        status(arg);
        return;
    }
    if (!strcmp(verb, "identify") && count <= 2 && valid_token(arg)) {
        identify_until = esp_timer_get_time() + 5000000;
        reply(arg, true, "flashing LED for 5 seconds");
        return;
    }
    if ((strcmp(verb, "role") && strcmp(verb, "transport")) ||
        count < 2 || count > 3 || !valid_token(token)) {
        reply("", false, "commands: status; identify; role rx|tx|both; transport wifi|ble; optional token");
        return;
    }
    uint8_t selected;
    bool change_role = !strcmp(verb, "role");
    if (change_role) {
        if (!strcmp(arg, "rx")) selected = BENCH_RX;
        else if (!strcmp(arg, "tx")) selected = BENCH_TX;
        else if (!strcmp(arg, "both")) selected = BENCH_BOTH;
        else { reply(token, false, "role must be rx, tx, or both"); return; }
    } else {
        if (!strcmp(arg, "wifi")) selected = BENCH_TX_WIFI;
        else if (!strcmp(arg, "ble")) selected = BENCH_TX_BLE;
        else { reply(token, false, "transport must be wifi or ble"); return; }
    }
    save_settings(change_role ? selected : (uint8_t)role,
                  change_role ? (uint8_t)tx_transport : selected, &identity, token);
}

static void usb_task(void *arg)
{
    (void)arg;
    char json[RID_JSON_SIZE];
    char line[192];
    size_t used = 0;
    bool discard_line = false;
    int64_t next_status = 0;
    uint8_t input[128];
    while (true) {
        int received = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(20));
        for (int i = 0; i < received; ++i) {
            if (input[i] == '\n') {
                if (!discard_line) { line[used] = '\0'; command(line); }
                else reply("", false, "command too long or contains non-ASCII bytes");
                used = 0;
                discard_line = false;
            } else if (input[i] != '\r') {
                if (input[i] < 0x20 || input[i] > 0x7e || used == sizeof(line) - 1)
                    discard_line = true;
                if (!discard_line) line[used++] = (char)input[i];
            }
        }
        // Bound work so incoming observations cannot starve USB commands.
        rid_observation_t observation;
        for (unsigned i = 0; i < 8 && xQueueReceive(observations, &observation, 0) == pdTRUE; ++i) {
            size_t len = rid_observation_json(json, sizeof(json), &observation,
                                              receiver_id, session, esp_timer_get_time());
            if (len) send_record(json, len);
            else ++usb_drops;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_status) { status(""); next_status = now + 2000000; }
        if (!usb_serial_jtag_is_connected()) identify_until = 0;
        bool led_on = now < identify_until ? now % 200000 < 100000 : now % 2000000 < 100000;
        gpio_set_level(BOARD_LED_GPIO, led_on ? BOARD_LED_ON_LEVEL : BOARD_LED_OFF_LEVEL);
    }
}

void app_main(void)
{
    const gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led_config));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LED_GPIO, BOARD_LED_OFF_LEVEL));
    usb_serial_jtag_driver_config_t usb_config = {.rx_buffer_size = 512, .tx_buffer_size = 8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rc);
    ESP_ERROR_CHECK(nvs_open("droneid", NVS_READWRITE, &config));
    uint8_t stored_role = BENCH_RX;
    rc = nvs_get_u8(config, "role", &stored_role);
    if (rc != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(rc);
    role = stored_role <= BENCH_BOTH ? (bench_role_t)stored_role : BENCH_RX;
    uint8_t stored_transport = BENCH_TX_WIFI;
    rc = nvs_get_u8(config, "tx_transport", &stored_transport);
    if (rc != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(rc);
    tx_transport = stored_transport <= BENCH_TX_BLE
        ? (bench_tx_transport_t)stored_transport : BENCH_TX_WIFI;
    uint8_t mac[6];
    // The generic efuse helper returns EUI-64 on C5; request MAC-48 explicitly.
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY));
    rid_identity_default(&identity, mac);
    saved_settings_t saved = {0};
    size_t saved_size = sizeof(saved);
    rc = nvs_get_blob(config, "settings_v1", &saved, &saved_size);
    if (rc == ESP_OK && saved_size == sizeof(saved) && saved.version == 1 &&
        saved.role <= BENCH_BOTH && saved.transport <= BENCH_TX_BLE && rid_identity_valid(&saved.identity)) {
        role = (bench_role_t)saved.role;
        tx_transport = (bench_tx_transport_t)saved.transport;
        identity = saved.identity;
    } else if (rc != ESP_ERR_NVS_NOT_FOUND) {
        // A malformed/newer settings blob must not enable an unintended transmitter.
        role = BENCH_RX;
    }
    snprintf(receiver_id, sizeof(receiver_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(session, sizeof(session), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    fc_config_default(&fc_config);
    fc_config_t stored_uart;
    size_t uart_size=sizeof(stored_uart);
    rc=nvs_get_blob(config,"uart_v1",&stored_uart,&uart_size);
    if (rc==ESP_OK && uart_size==sizeof(stored_uart) && fc_config_valid(&stored_uart)) fc_config=stored_uart;
    // Missing/corrupt settings leave UART disabled, preserving the USB bench.
    fc_uart_start(&fc_config,mac,strtoull(session,NULL,16),&radio_stats,&usb_drops);
    observations = xQueueCreate(64, sizeof(rid_observation_t));
    ESP_ERROR_CHECK(observations ? ESP_OK : ESP_ERR_NO_MEM);
    bench_radio_start(role, tx_transport, &identity, observations, &radio_stats);
    ESP_ERROR_CHECK(xTaskCreate(usb_task, "usb_bench", 12288, NULL, 5, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
