#pragma once
#include "bench.h"
#include "fc_link.h"

void fc_uart_start(const fc_config_t *config, const uint8_t mac[6], uint64_t session,
                   bench_radio_stats_t *radio, const _Atomic uint32_t *usb_drops);
void fc_uart_submit(const rid_observation_t *observation);
// One nested JSON object for the USB status stream; zero on insufficient space.
size_t fc_uart_json(char *out, size_t capacity, uint64_t now);
