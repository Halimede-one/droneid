// SPDX-License-Identifier: BSD-2-Clause
// DroneID FC ingress policy. No dependency on a vendor MAVLink parser.
// Zero-initialize one state per protected physical UART. Trusted build policy
// only: these IDs are format checks, never authentication credentials.
#ifndef DRONEID_FC_FILTER_H
#define DRONEID_FC_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DRONEID_FC_SERIAL 4
#define DRONEID_FC_SYSTEM 1
#define DRONEID_FC_SENSOR_COMPONENT 242
#define DRONEID_FC_GUARD_COMPONENT 243
#define DRONEID_FC_DEST_SYSTEM 255
#define DRONEID_FC_DEST_COMPONENT 190
#define DRONEID_FC_READ_LIMIT 128U
#define DRONEID_FC_FRAME_MAX 145U

typedef struct {
    uint32_t credit, updated_ms;
} droneid_fc_budget_t;

typedef struct {
    uint8_t frame[DRONEID_FC_FRAME_MAX];
    uint16_t used, expected;
    uint8_t sequence;
    uint32_t first_ms, last_ms, accepted_ms[3];
    bool initialized, seen[3];
    droneid_fc_budget_t input, output;
    // Saturating diagnostic counters; accepted means policy admission, not
    // downstream signature acceptance or delivery to the operator.
    uint32_t input_bytes, dropped_bytes, rejected_frames, accepted[3];
} droneid_fc_filter_t;

static inline void droneid_fc_count(uint32_t *value)
{
    if (*value != UINT32_MAX) ++*value;
}

static inline bool droneid_fc_credit(droneid_fc_budget_t *b, uint32_t now,
                                     uint32_t rate, uint32_t burst, uint32_t bytes)
{
    uint32_t elapsed = now - b->updated_ms; // Includes uint32 millisecond wrap.
    if (elapsed > 1000U) elapsed = 1000U;
    uint32_t credit = b->credit + elapsed * rate;
    b->credit = credit > burst * 1000U ? burst * 1000U : credit;
    b->updated_ms = now;
    if (b->credit < bytes * 1000U) return false;
    b->credit -= bytes * 1000U;
    return true;
}

static inline uint16_t droneid_fc_crc(uint16_t crc, uint8_t byte)
{
    uint8_t t = (uint8_t)(byte ^ (uint8_t)crc);
    t ^= (uint8_t)(t << 4);
    return (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
}

static inline int droneid_fc_base64(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static inline uint64_t droneid_fc_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

// Returns heartbeat=0, observation=1, health=2, invalid=-1.
// CRC, shape and metadata checks are independent of the radio and guard code.
static inline int droneid_fc_record(const uint8_t *f, unsigned length, uint8_t record[84])
{
    if (length < 21 || length > DRONEID_FC_FRAME_MAX || f[0] != 0xfd || f[2] || f[3] ||
        f[5] != DRONEID_FC_SYSTEM ||
        (f[6] != DRONEID_FC_SENSOR_COMPONENT && f[6] != DRONEID_FC_GUARD_COMPONENT) ||
        length != (unsigned)f[1] + 12U) return -1;
    bool direct = f[6] == DRONEID_FC_SENSOR_COMPONENT;
    if (!direct) {
        for (unsigned i = 1; i < length; ++i) if (f[i] == 0xfd || f[i] == 0xfe) return -1;
    }
    bool heartbeat = f[7] == 0 && f[8] == 0 && f[9] == 0;
    bool tunnel = f[7] == 0x81 && f[8] == 1 && f[9] == 0;
    if (!heartbeat && !tunnel) return -1;
    uint16_t crc = 0xffff;
    for (unsigned i = 1; i < length - 2; ++i) crc = droneid_fc_crc(crc, f[i]);
    crc = droneid_fc_crc(crc, heartbeat ? 50 : 147);
    if (f[length - 2] != (uint8_t)crc || f[length - 1] != (uint8_t)(crc >> 8)) return -1;
    const uint8_t *p = f + 10;
    if (heartbeat) {
        static const uint8_t expected[9] = {0, 0, 0, 0, 18, 8, 0, 4, 3};
        return f[1] == 9 && memcmp(p, expected, sizeof(expected)) == 0 ? 0 : -1;
    }
    if (p[0] || p[1] || p[2] != DRONEID_FC_DEST_SYSTEM || p[3] != DRONEID_FC_DEST_COMPONENT) return -1;
    unsigned decoded = 0;
    if (direct) {
        // Restore MAVLink 2's omitted trailing zeros. The radio's 128-byte
        // TUNNEL array may be trimmed, but its declared record is always 78/84.
        if (f[1] < 11 || (p[4] != 78 && p[4] != 84)) return -1;
        decoded = p[4];
        memset(record, 0, 84);
        unsigned received = f[1] - 5U;
        memcpy(record, p + 5, received < decoded ? received : decoded);
        for (unsigned i = decoded; i < received; ++i) if (p[5 + i]) return -1;
    } else {
        if ((f[1] != 113 && f[1] != 121) || p[4] != f[1] - 5 || memcmp(p + 5, "DRG2", 4)) return -1;
        for (unsigned i = 9; i < f[1]; i += 4) {
            uint32_t bits = 0;
            for (unsigned j = 0; j < 4; ++j) {
                int v = droneid_fc_base64(p[i + j]);
                if (v < 0) return -1;
                bits = (bits << 6) | (unsigned)v;
            }
            record[decoded++] = (uint8_t)(bits >> 16);
            record[decoded++] = (uint8_t)(bits >> 8);
            record[decoded++] = (uint8_t)bits;
        }
    }
    // Both supported lengths are divisible by three: padding, hidden suffixes,
    // whitespace and alternative Base64 encodings are never accepted.
    if (memcmp(record, "DRID", 4) || record[4] != 1 ||
        droneid_fc_u64(record + 24) > droneid_fc_u64(record + 32)) return -1;
    if (decoded == 84 && record[5] == 2)
        return droneid_fc_u64(record + 24) == droneid_fc_u64(record + 32) ? 2 : -1;
    if (decoded != 78 || record[5] != 1 || record[40] > 2 || record[41] == 127 ||
        record[52] < 1 || record[52] > 9 || record[51] >= record[52]) return -1;
    if (record[40] == 0)
        return record[48] <= 3 && record[49] == 0 && record[51] == 0 && record[52] == 1 ? 1 : -1;
    return record[48] == 255 && (record[49] == 6 || record[49] == 149) ? 1 : -1;
}

// Encode accepted data BEFORE native parsing/routing. This also protects
// downstream parsers from nested frame starts in the original raw RID record.
// The component remains 242 (direct radio) or 243 (external guard); an encoded
// report is not evidence that a second XIAO exists or that its sender is trusted.
static inline unsigned droneid_fc_normalize(droneid_fc_filter_t *s, const uint8_t record[84], unsigned kind)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t component = s->frame[6];
    uint8_t *f = s->frame;
    memset(f, 0, DRONEID_FC_FRAME_MAX);
    f[0] = 0xfd; f[5] = DRONEID_FC_SYSTEM; f[6] = component;
    if (kind == 0) {
        static const uint8_t heartbeat[9] = {0, 0, 0, 0, 18, 8, 0, 4, 3};
        f[1] = 9;
        memcpy(f + 10, heartbeat, 9);
    } else {
        unsigned size = kind == 1 ? 78 : 84;
        f[1] = kind == 1 ? 113 : 121;
        f[7] = 0x81; f[8] = 1;
        f[12] = DRONEID_FC_DEST_SYSTEM; f[13] = DRONEID_FC_DEST_COMPONENT;
        f[14] = f[1] - 5;
        memcpy(f + 15, "DRG2", 4);
        unsigned at = 19;
        for (unsigned i = 0; i < size; i += 3) {
            uint32_t bits = ((uint32_t)record[i] << 16) | ((uint32_t)record[i + 1] << 8) | record[i + 2];
            f[at++] = (uint8_t)alphabet[(bits >> 18) & 63]; f[at++] = (uint8_t)alphabet[(bits >> 12) & 63];
            f[at++] = (uint8_t)alphabet[(bits >> 6) & 63]; f[at++] = (uint8_t)alphabet[bits & 63];
        }
    }
    unsigned length = f[1] + 12U;
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        f[4] = s->sequence++;
        uint16_t crc = 0xffff;
        for (unsigned i = 1; i < length - 2; ++i) crc = droneid_fc_crc(crc, f[i]);
        crc = droneid_fc_crc(crc, kind == 0 ? 50 : 147);
        f[length - 2] = (uint8_t)crc; f[length - 1] = (uint8_t)(crc >> 8);
        bool clean = true;
        for (unsigned i = 1; i < length; ++i) if (f[i] == 0xfd || f[i] == 0xfe) { clean = false; break; }
        if (clean) return length;
    }
    return 0;
}

// Each call consumes one byte. Returns a complete NORMALIZED frame's length,
// held in state->frame until the next call. No raw record or partial frame is
// exposed to the vendor parser. Adapters read <=128 bytes per scheduler call.
static inline unsigned droneid_fc_feed(droneid_fc_filter_t *s, uint8_t c, uint32_t now)
{
    if (!s->initialized) {
        s->initialized = true;
        s->input.credit = 512000;
        s->output.credit = 160000;
        s->input.updated_ms = s->output.updated_ms = now;
    }
    droneid_fc_count(&s->input_bytes);
    if (!droneid_fc_credit(&s->input, now, 4096, 512, 1)) {
        s->used = 0;
        droneid_fc_count(&s->dropped_bytes);
        return 0;
    }
    if (s->used && (now - s->last_ms > 50U || now - s->first_ms > 100U)) s->used = 0;
    s->last_ms = now;
    if (!s->used && c == 0xfd) {
        s->used = 1;
        s->expected = 0;
        s->first_ms = now;
        s->frame[0] = c;
        return 0;
    }
    if (!s->used) return 0;
    // While collecting a frame, 0xfd/0xfe are ordinary data, including CRC
    // and sequence bytes. Never rescan a rejected frame's body for commands.
    s->frame[s->used++] = c;
    if (s->used == 2) {
        if (c < 9 || c > 133) {
            s->used = 0;
            droneid_fc_count(&s->rejected_frames);
        } else s->expected = (uint16_t)c + 12;
        return 0;
    }
    if (s->used < s->expected) return 0;
    unsigned length = s->used;
    s->used = 0;
    uint8_t record[84];
    int kind = droneid_fc_record(s->frame, length, record);
    static const uint32_t periods[3] = {1000, 200, 2000};
    if (kind < 0 || (s->seen[kind] && now - s->accepted_ms[kind] < periods[kind])) {
        droneid_fc_count(&s->rejected_frames);
        return 0;
    }
    length = droneid_fc_normalize(s, record, (unsigned)kind);
    if (!length || !droneid_fc_credit(&s->output, now, 800, 160, length)) {
        droneid_fc_count(&s->rejected_frames);
        return 0;
    }
    s->seen[kind] = true;
    s->accepted_ms[kind] = now;
    droneid_fc_count(&s->accepted[kind]);
    return length;
}
#endif
