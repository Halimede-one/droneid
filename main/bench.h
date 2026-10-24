#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rid.h"

typedef enum { BENCH_RX = 0, BENCH_TX = 1, BENCH_BOTH = 2 } bench_role_t;
typedef enum { BENCH_TX_WIFI = 0, BENCH_TX_BLE = 1 } bench_tx_transport_t;
typedef enum { BENCH_LISTEN_IDLE = 0, BENCH_LISTEN_BLE, BENCH_LISTEN_WIFI6, BENCH_LISTEN_WIFI149 } bench_listen_t;
typedef struct {
    _Atomic uint32_t advertisements;
    _Atomic uint32_t observations;
    _Atomic uint32_t malformed;
    _Atomic uint32_t queue_drops;
    _Atomic uint32_t tx_sets;
    _Atomic uint32_t radio_errors;
    _Atomic bool ready;
    _Atomic bool ble_ready;
    _Atomic bool wifi_ready;
    _Atomic uint32_t ble_observations;
    _Atomic uint32_t wifi_observations;
    _Atomic uint32_t wifi_frames;
    _Atomic uint32_t wifi_tx_frames;
    _Atomic uint32_t scan_cycles;
    _Atomic uint32_t listen_ms[4]; // Completed windows only, cumulative, may wrap.
    _Atomic unsigned listen_phase;
} bench_radio_stats_t;

void bench_radio_start(bench_role_t role, bench_tx_transport_t tx_transport, const rid_identity_t *identity,
                       QueueHandle_t observations, bench_radio_stats_t *stats);
void bench_submit(rid_observation_t *observation);
void bench_ble_start(const rid_identity_t *identity, bench_radio_stats_t *stats);
bool bench_ble_scan(bool enable);
bool bench_ble_advertise(bool enable, bool self_id);
void bench_wifi_start(const rid_identity_t *identity, bench_radio_stats_t *stats);
bool bench_wifi_listen(uint8_t channel); // Zero disables promiscuous reception.
bool bench_wifi_transmit(void); // Wi-Fi Beacon, channel 6.
