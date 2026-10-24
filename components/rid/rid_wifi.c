#include "rid.h"
#include <string.h>

// Wire layouts follow the pinned OpenDroneID wifi.c. Read lengths bytewise:
// received 802.11 headers/IEs need not be aligned and are entirely untrusted.
static int pack(const uint8_t *p, size_t len, rid_wifi_packet_t *out)
{
    // Counter, message-pack header, single-message size, count.
    if (len < 4 || (p[1] >> 4) != ODID_MESSAGETYPE_PACKED ||
        (p[1] & 15) > ODID_PROTOCOL_VERSION || p[2] != ODID_MESSAGE_SIZE ||
        p[3] == 0 || p[3] > ODID_PACK_MAX_MESSAGES ||
        len != 4u + (size_t)p[3] * ODID_MESSAGE_SIZE) return -1;
    out->count = p[3];
    for (unsigned i = 0; i < out->count; ++i) {
        out->messages[i].counter = p[0];
        memcpy(out->messages[i].message, p + 4 + i * ODID_MESSAGE_SIZE, ODID_MESSAGE_SIZE);
    }
    return 1;
}

int rid_parse_wifi(const uint8_t *frame, size_t len, rid_wifi_packet_t *out)
{
    if (!frame || !out) return -1;
    if (len < 2) return 0;
    // Management, protocol zero, beacon or action. No protected/fragmented data.
    if ((frame[0] != 0x80 && frame[0] != 0xd0) || (frame[1] & 0xc7)) return 0;
    if (len < 24) return -1;
    if (frame[22] & 15) return 0;
    rid_wifi_packet_t candidate = {0};
    memcpy(candidate.address, frame + 10, 6);
    bool found = false;
    if (frame[0] == 0x80) {
        candidate.transport = RID_WIFI_BEACON;
        if (len < 36) return -1;
        for (size_t pos = 36; pos < len;) {
            if (len - pos < 2) return -1;
            unsigned id = frame[pos++];
            size_t n = frame[pos++];
            if (n > len - pos) return -1;
            const uint8_t oui[] = {0xfa, 0x0b, 0xbc, 0x0d};
            if (id == 221 && n >= sizeof(oui) && !memcmp(frame + pos, oui, sizeof(oui))) {
                if (found || pack(frame + pos + 4, n - 4, &candidate) != 1) return -1;
                found = true;
            }
            pos += n;
        }
    } else {
        const uint8_t nan[] = {0x04, 0x09, 0x50, 0x6f, 0x9a, 0x13};
        const uint8_t service[] = {0x88, 0x69, 0x19, 0x9d, 0x92, 0x09};
        candidate.transport = RID_WIFI_NAN;
        if (len < 30) return -1;
        if (memcmp(frame + 24, nan, sizeof(nan))) return 0;
        for (size_t pos = 30; pos < len;) {
            if (len - pos < 3) return -1;
            unsigned id = frame[pos++];
            size_t n = (size_t)frame[pos] | ((size_t)frame[pos + 1] << 8);
            pos += 2;
            if (n > len - pos) return -1;
            const uint8_t *attr = frame + pos;
            if (id == 3 && n >= 6 && !memcmp(attr, service, sizeof(service))) {
                if (n < 10) return -1;
                // Remote ID's NAN service descriptor uses Service Info Present
                // with no optional match/response filters. Other profiles skip.
                if (attr[8] != 0x10) return 0;
                if (found || (size_t)attr[9] != n - 10 ||
                    pack(attr + 10, n - 10, &candidate) != 1) return -1;
                found = true;
            }
            pos += n;
        }
    }
    if (found) *out = candidate;
    return found ? 1 : 0;
}

size_t rid_identity_wifi_beacon(const rid_identity_t *identity, const uint8_t source[6],
                            uint8_t counter, uint64_t timestamp_us,
                            uint8_t *out, size_t capacity)
{
    static const uint8_t ssid[] = "DroneID-BENCH";
    const size_t size = 36 + 2 + sizeof(ssid) - 1 + 3 + 3 + 2 + 8 + 2 * ODID_MESSAGE_SIZE;
    if (!source || !out || capacity < size) return 0;
    uint8_t basic[RID_ADV_SIZE], self[RID_ADV_SIZE];
    if (!rid_identity_advertisement(identity, false, counter, basic) ||
        !rid_identity_advertisement(identity, true, counter, self)) return 0;
    memset(out, 0, size);
    out[0] = 0x80;
    memset(out + 4, 0xff, 6);
    memcpy(out + 10, source, 6);
    memcpy(out + 16, source, 6);
    for (unsigned i = 0; i < 8; ++i) out[24 + i] = timestamp_us >> (8 * i);
    out[32] = 100; // Beacon interval in TU (102.4 ms), bench transmissions repeat.
    out[34] = 0x20; out[35] = 0x04; // Short preamble/slot time, no AP association.
    size_t pos = 36;
    out[pos++] = 0; out[pos++] = sizeof(ssid) - 1;
    memcpy(out + pos, ssid, sizeof(ssid) - 1); pos += sizeof(ssid) - 1;
    out[pos++] = 1; out[pos++] = 1; out[pos++] = 0x8c; // Basic rate 6 Mbps.
    out[pos++] = 3; out[pos++] = 1; out[pos++] = 6; // Transmit channel 6 only.
    out[pos++] = 221; out[pos++] = 8 + 2 * ODID_MESSAGE_SIZE;
    out[pos++] = 0xfa; out[pos++] = 0x0b; out[pos++] = 0xbc; out[pos++] = 0x0d;
    out[pos++] = counter;
    out[pos++] = (ODID_MESSAGETYPE_PACKED << 4) | ODID_PROTOCOL_VERSION;
    out[pos++] = ODID_MESSAGE_SIZE; out[pos++] = 2;
    memcpy(out + pos, basic + 6, ODID_MESSAGE_SIZE); pos += ODID_MESSAGE_SIZE;
    memcpy(out + pos, self + 6, ODID_MESSAGE_SIZE); pos += ODID_MESSAGE_SIZE;
    return pos;
}

size_t rid_bench_wifi_beacon(const uint8_t mac[6], const uint8_t source[6],
                            uint8_t counter, uint64_t timestamp_us,
                            uint8_t *out, size_t capacity)
{
    if (!mac) return 0;
    rid_identity_t identity;
    rid_identity_default(&identity, mac);
    return rid_identity_wifi_beacon(&identity, source, counter, timestamp_us, out, capacity);
}
