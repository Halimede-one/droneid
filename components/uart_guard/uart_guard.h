#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/mavlink.h"

// Run this policy on an independent processor, never inside the radio sensor
// as its only containment. Configuration is trusted, local build-time policy.
#define GUARD_INPUT_CHUNK 128
#define GUARD_PENDING_US UINT64_C(2000000)
#define GUARD_INPUT_RATE 4096
#define GUARD_INPUT_BURST 512
#define GUARD_FC_OUTPUT_RATE 800
#define GUARD_SENSOR_OUTPUT_RATE 500
#define GUARD_OUTPUT_BURST 160

typedef enum { GUARD_SENSOR = 0, GUARD_FC = 1 } guard_port_t;

typedef struct {
    uint8_t system_id, sensor_component, guard_component, fc_component;
    uint8_t target_system, target_component;
} guard_config_t;

typedef struct {
    uint64_t credit, updated_us;
    uint32_t rate, burst;
} guard_budget_t;

typedef struct {
    mavlink_message_t frame;
    mavlink_status_t status;
    uint64_t last_byte_us;
} guard_parser_t;

typedef struct {
    uint8_t data[128];
    uint8_t length;
    uint32_t message_id;
    uint64_t received_us;
    bool present;
} guard_pending_t;

typedef struct {
    uint32_t input_bytes[2], input_dropped[2], framing_errors[2];
    uint32_t rejected[2], accepted[2], replaced[2], expired[2];
    uint32_t output_frames[2], output_bytes[2], encode_errors;
} guard_counters_t;

typedef struct {
    guard_config_t config;
    guard_parser_t parser[2];
    guard_budget_t input_budget[2], output_budget[2];
    // Latest observation + health, and latest FC heartbeat + position + GPS.
    guard_pending_t sensor[2], fc[3];
    uint64_t next_fc_us[3], next_sensor_us[3], fc_heartbeat_us;
    uint8_t sequence[2];
    bool enabled, fc_heartbeat_seen;
    guard_counters_t counters;
} uart_guard_t;

void guard_config_default(guard_config_t *config);
bool guard_config_valid(const guard_config_t *config);
bool guard_init(uart_guard_t *guard, const guard_config_t *config, uint64_t now_us);
// Work is capped at GUARD_INPUT_CHUNK per call and by a per-source byte budget.
// Returns the number consumed; caller discards the rest, never retries it.
size_t guard_ingest(uart_guard_t *guard, guard_port_t source,
                    const uint8_t *input, size_t length, uint64_t now_us);
// Generates at most one complete, regenerated frame for this destination.
// Output must have MAVLINK_MAX_PACKET_LEN bytes. Never exposes partial input.
size_t guard_poll(uart_guard_t *guard, guard_port_t destination,
                  uint64_t now_us, uint8_t output[MAVLINK_MAX_PACKET_LEN]);
// UART errors invalidate partial input and pending records from that source.
void guard_input_fault(uart_guard_t *guard, guard_port_t source);
