#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opendroneid.h"

#define RID_ADV_SIZE 31
#define RID_JSON_SIZE 2048
#define RID_WIFI_FRAME_SIZE 160
#define RID_BENCH_ID_MAX 19
#define RID_HELLO_MAX 23

typedef struct {
    char id[RID_BENCH_ID_MAX + 1];
    char hello[RID_HELLO_MAX + 1];
} rid_identity_t;

void rid_identity_default(rid_identity_t *identity, const uint8_t mac[6]);
bool rid_identity_valid(const rid_identity_t *identity);
bool rid_identity_from_hex(rid_identity_t *out, const char *id_hex, const char *hello_hex);
bool rid_identity_advertisement(const rid_identity_t *identity, bool self_id,
                                uint8_t counter, uint8_t out[RID_ADV_SIZE]);
size_t rid_identity_wifi_beacon(const rid_identity_t *identity, const uint8_t source[6],
                               uint8_t counter, uint64_t timestamp_us,
                               uint8_t *out, size_t capacity);

typedef enum { RID_BLE_LEGACY = 0, RID_WIFI_BEACON, RID_WIFI_NAN } rid_transport_t;

typedef struct {
    uint8_t message[ODID_MESSAGE_SIZE];
    uint8_t counter;
} rid_payload_t;

typedef struct {
    rid_payload_t payload;
    uint8_t address[6]; // Display/network order, not NimBLE's reversed order.
    uint8_t address_type;
    int8_t rssi;
    uint64_t rx_us;
    uint32_t seq;
    rid_transport_t transport;
    uint8_t channel; // Wi-Fi channel; zero means unavailable.
    uint8_t pack_index;
    uint8_t pack_size; // Zero is treated as one for legacy callers.
} rid_observation_t;

typedef struct {
    rid_transport_t transport;
    uint8_t address[6];
    uint8_t count;
    rid_payload_t messages[ODID_PACK_MAX_MESSAGES];
} rid_wifi_packet_t;

// Frames exclude the four-byte FCS. 1 = RID pack, 0 = unrelated, -1 = malformed.
// Output is untouched unless the entire frame/pack validates.
int rid_parse_wifi(const uint8_t *frame, size_t len, rid_wifi_packet_t *out);
size_t rid_bench_wifi_beacon(const uint8_t mac[6], const uint8_t source[6],
                            uint8_t counter, uint64_t timestamp_us,
                            uint8_t *out, size_t capacity);

// 1 = one legacy RID message, 0 = unrelated advertisement, -1 = malformed.
int rid_parse_legacy(const uint8_t *adv, size_t len, rid_payload_t *out);
bool rid_bench_advertisement(const uint8_t mac[6], bool self_id,
                             uint8_t counter, uint8_t out[RID_ADV_SIZE]);
// Produces a complete JSON line, or returns zero (never a truncated record).
size_t rid_observation_json(char *out, size_t capacity,
                            const rid_observation_t *observation,
                            const char *receiver_id, const char *session,
                            uint64_t reported_us);
