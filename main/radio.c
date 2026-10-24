#include "bench.h"
#include "fc_uart.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"

static bench_role_t role;
static bench_tx_transport_t tx_transport;
static bench_radio_stats_t *stats;
static QueueHandle_t queue;

void bench_submit(rid_observation_t *o)
{
    o->seq = atomic_fetch_add(&stats->observations, 1) + 1;
    if (o->transport == RID_BLE_LEGACY) atomic_fetch_add(&stats->ble_observations, 1);
    else atomic_fetch_add(&stats->wifi_observations, 1);
    fc_uart_submit(o);
    if (xQueueSend(queue, o, 0) != pdTRUE) atomic_fetch_add(&stats->queue_drops, 1);
}

static void pause_ms(unsigned ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void window_end(bench_listen_t phase, int64_t started)
{
    atomic_fetch_add(&stats->listen_ms[phase], (uint32_t)((esp_timer_get_time() - started) / 1000));
    atomic_store(&stats->listen_phase, BENCH_LISTEN_IDLE);
}

static void radio_worker(void *unused)
{
    (void)unused;
    bool need_ble = role != BENCH_TX || tx_transport == BENCH_TX_BLE;
    bool need_wifi = role != BENCH_TX || tx_transport == BENCH_TX_WIFI;
    for (;;) {
        bool ready = (!need_ble || atomic_load(&stats->ble_ready)) &&
                     (!need_wifi || atomic_load(&stats->wifi_ready));
        atomic_store(&stats->ready, ready);
        if (!ready) { pause_ms(100); continue; }
        if (role == BENCH_TX) {
            if (tx_transport == BENCH_TX_WIFI) {
                bench_wifi_transmit(); pause_ms(100);
            } else {
                bench_ble_advertise(true, false); pause_ms(500);
                bench_ble_advertise(true, true); pause_ms(500);
            }
            continue;
        }
        // Randomize channel order so independently booted peers do not spend
        // many cycles with non-overlapping channel-6 transmit/receive windows.
        // No BLE procedure runs during either Wi-Fi sniffing window.
        unsigned first_wifi6_slot = esp_random() & 1;
        for (unsigned slot = 0; slot < 2; ++slot) {
            bool wifi6 = slot == first_wifi6_slot;
            uint8_t channel = wifi6 ? 6 : 149;
            bench_listen_t phase = wifi6 ? BENCH_LISTEN_WIFI6 : BENCH_LISTEN_WIFI149;
            if (bench_wifi_listen(channel)) {
                atomic_store(&stats->listen_phase, phase);
                int64_t started = esp_timer_get_time();
                for (unsigned i = 0; i < 3; ++i) {
                    if (wifi6 && role == BENCH_BOTH && tx_transport == BENCH_TX_WIFI)
                        bench_wifi_transmit();
                    pause_ms(100);
                }
                window_end(phase, started);
            } else pause_ms(300);
        }
        if (!bench_wifi_listen(0)) { pause_ms(100); continue; }
        if (bench_ble_scan(true)) {
            atomic_store(&stats->listen_phase, BENCH_LISTEN_BLE);
            int64_t started = esp_timer_get_time();
            if (role == BENCH_BOTH && tx_transport == BENCH_TX_BLE)
                bench_ble_advertise(true, false);
            pause_ms(200);
            if (role == BENCH_BOTH && tx_transport == BENCH_TX_BLE)
                bench_ble_advertise(true, true);
            // Vary cycle length to avoid permanent alignment with a peer's
            // periodic broadcasts/windows. These are nominal dwell times.
            pause_ms(200 + esp_random() % 101);
            window_end(BENCH_LISTEN_BLE, started);
        }
        // Stay out of Wi-Fi windows until both BLE procedures have stopped.
        while (!bench_ble_scan(false) || !bench_ble_advertise(false, false)) pause_ms(100);
        atomic_fetch_add(&stats->scan_cycles, 1);
    }
}

void bench_radio_start(bench_role_t selected_role, bench_tx_transport_t selected_transport,
                       const rid_identity_t *identity, QueueHandle_t observations,
                       bench_radio_stats_t *radio_stats)
{
    role = selected_role;
    tx_transport = selected_transport;
    stats = radio_stats;
    queue = observations;
    if (role != BENCH_TX || tx_transport == BENCH_TX_WIFI) bench_wifi_start(identity, stats);
    if (role != BENCH_TX || tx_transport == BENCH_TX_BLE) bench_ble_start(identity, stats);
    ESP_ERROR_CHECK(xTaskCreate(radio_worker, "rid_radio", 4096, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
