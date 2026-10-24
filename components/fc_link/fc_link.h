#pragma once

#include "rid.h"
#include "common/mavlink.h"

#define FC_OBSERVATION_SIZE 78
#define FC_HEALTH_SIZE 84
#define FC_STALE_US UINT64_C(5000000)

// Persist separately from the radio identity. All bytes are initialized.
typedef struct {
    uint32_t baud;
    uint8_t version, enabled, tx_pin, rx_pin;
    uint8_t system_id, component_id, fc_component_id;
    uint8_t target_system, target_component, max_rate;
    uint8_t reserved[2];
} fc_config_t;

typedef struct {
    uint64_t heartbeat_us, position_us, gps_us;
    uint32_t custom_mode;
    int32_t lat, lon, altitude_mm, relative_altitude_mm;
    uint16_t heading_cdeg;
    uint8_t base_mode, autopilot, vehicle_type, fix_type, satellites;
    bool heartbeat_seen, position_seen, gps_seen;
    uint32_t rx_frames, parse_errors, signed_ignored;
} fc_telemetry_t;

typedef struct {
    fc_config_t config;
    mavlink_status_t tx_status, rx_status;
    mavlink_message_t rx_message;
    fc_telemetry_t telemetry;
    uint8_t receiver_mac[6];
    uint64_t session;
} fc_link_t;

void fc_config_default(fc_config_t *config);
bool fc_config_valid(const fc_config_t *config);
bool fc_config_parse(fc_config_t *out, const char *text);
void fc_link_init(fc_link_t *link, const fc_config_t *config, const uint8_t mac[6], uint64_t session);
// One owner calls all link functions. No radio or USB driver dependencies.
size_t fc_heartbeat(fc_link_t *link, uint8_t out[MAVLINK_MAX_PACKET_LEN]);
size_t fc_observation(fc_link_t *link, const rid_observation_t *observation, uint64_t now,
                      uint8_t out[MAVLINK_MAX_PACKET_LEN]);
size_t fc_health(fc_link_t *link, uint32_t sequence, uint64_t now, const uint32_t counters[11],
                 uint8_t out[MAVLINK_MAX_PACKET_LEN]);
void fc_receive(fc_link_t *link, const uint8_t *bytes, size_t len, uint64_t now);
bool fc_heartbeat_fresh(const fc_telemetry_t *telemetry, uint64_t now);
