#include "droneid_fc_filter.h"
#include "uart_guard.h"
#include <assert.h>
#include <stdio.h>

static unsigned feed(droneid_fc_filter_t *s, const uint8_t *p, size_t length, uint32_t now)
{
    unsigned admitted = 0;
    for (size_t i = 0; i < length; ++i) if (droneid_fc_feed(s, p[i], now)) ++admitted;
    return admitted;
}

static bool allowed(const uint8_t *p, size_t n)
{
    droneid_fc_filter_t s = {0};
    return feed(&s, p, n, 0) != 0;
}

static size_t frame(mavlink_message_t *m, uint8_t *output, uint8_t extra, unsigned sys, unsigned comp)
{
    // Canonical guard framing, including its no-interior-start-marker rule.
    for (unsigned seq = 0; seq < 256; ++seq) {
        mavlink_status_t status = {0};
        status.current_tx_seq = seq;
        mavlink_finalize_message_buffer(m, sys, comp, &status, m->len, m->len, extra);
        size_t length = mavlink_msg_to_send_buffer(output, m);
        bool clean = true;
        for (size_t i = 1; i < length; ++i) if (output[i] == 0xfd || output[i] == 0xfe) clean = false;
        if (clean) return length;
    }
    assert(false);
    return 0;
}

static size_t report(uint8_t *output, unsigned kind, int bad_offset, uint8_t bad_value)
{
    // Construct independently from the guard's encoder.
    uint8_t record[84] = {0};
    memcpy(record, "DRID", 4);
    record[4] = 1; record[5] = kind;
    record[40] = 1; record[41] = (uint8_t)-55;
    record[48] = 255; record[49] = 6; record[52] = 1;
    if (bad_offset >= 0) record[bad_offset] = bad_value;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t data[128] = {0};
    memcpy(data, "DRG2", 4);
    unsigned at = 4, length = kind == 1 ? 78 : 84;
    for (unsigned i = 0; i < length; i += 3) {
        uint32_t bits = ((uint32_t)record[i] << 16) | ((uint32_t)record[i+1] << 8) | record[i+2];
        data[at++] = alphabet[(bits >> 18) & 63]; data[at++] = alphabet[(bits >> 12) & 63];
        data[at++] = alphabet[(bits >> 6) & 63]; data[at++] = alphabet[bits & 63];
    }
    mavlink_message_t m;
    mavlink_status_t status = {0};
    mavlink_msg_tunnel_pack_status(1, 243, &status, &m, 255, 190, 0, at, data);
    return frame(&m, output, MAVLINK_MSG_ID_TUNNEL_CRC, 1, 243);
}

int main(void)
{
    uint8_t good[MAVLINK_MAX_PACKET_LEN], altered[MAVLINK_MAX_PACKET_LEN], health[MAVLINK_MAX_PACKET_LEN];
    size_t n = report(good, 1, -1, 0), hn = report(health, 2, -1, 0);
    assert(n == 125 && hn == 133 && allowed(good, n) && allowed(health, hn));
    // Every one-byte corruption fails closed (including payload, flags and CRC).
    for (size_t i = 0; i < n; ++i) {
        memcpy(altered, good, n); altered[i] ^= 1;
        assert(!allowed(altered, n));
    }
    // A parser starting at any interior offset cannot accept a nested frame.
    for (size_t i = 1; i < n; ++i) assert(!allowed(good + i, n - i));
    // Independently valid CRC does not turn bad record metadata into a report.
    const unsigned offsets[] = {0, 4, 5, 24, 40, 41, 48, 49, 51, 52};
    const uint8_t values[] = {'X', 2, 3, 1, 3, 127, 0, 1, 1, 0};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        size_t length = report(altered, 1, offsets[i], values[i]);
        assert(!allowed(altered, length));
    }
    assert(!allowed(altered, report(altered, 2, 32, 1)));
    // Validate every other known common-dialect message type under spoofed
    // guard IDs. Correct CRC/source is never sufficient for admission.
    static const mavlink_msg_entry_t entries[] = MAVLINK_MESSAGE_CRCS;
    for (unsigned i = 0; i < sizeof(entries)/sizeof(entries[0]); ++i) {
        const mavlink_msg_entry_t *entry = &entries[i];
        if (entry->msgid == 0 || entry->msgid == 385) continue;
        mavlink_message_t m = {0};
        m.msgid = entry->msgid; m.len = entry->max_msg_len;
        mavlink_status_t status = {0};
        mavlink_finalize_message_buffer(&m, 1, 243, &status, entry->min_msg_len, entry->max_msg_len, entry->crc_extra);
        size_t length = mavlink_msg_to_send_buffer(altered, &m);
        assert(!allowed(altered, length));
    }
    mavlink_message_t m;
    mavlink_status_t status = {0};
    mavlink_msg_heartbeat_pack_status(1, 243, &status, &m, MAV_TYPE_ONBOARD_CONTROLLER,
        MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    size_t bn = frame(&m, altered, 50, 1, 243);
    assert(allowed(altered, bn));
    // FC/GCS impersonation, noncanonical heartbeat and MAVLink 1.
    assert(!allowed(altered, frame(&m, altered, 50, 1, 1)));
    assert(!allowed(altered, frame(&m, altered, 50, 255, 190)));
    _MAV_PAYLOAD_NON_CONST(&m)[6] = MAV_MODE_FLAG_SAFETY_ARMED;
    assert(!allowed(altered, frame(&m, altered, 50, 1, 243)));
    // Valid outer CRC but illegal destination/type/encoding/flags/suffix.
    for (unsigned which = 0; which < 8; ++which) {
        mavlink_message_t parsed = {0}; mavlink_status_t ps = {0};
        for (size_t i = 0; i < n; ++i) mavlink_frame_char_buffer(&parsed, &ps, good[i], &m, &status);
        uint8_t *p = (uint8_t *)_MAV_PAYLOAD_NON_CONST(&m);
        if (which == 0) p[2] = 0;
        if (which == 1) p[3] = 0;
        if (which == 2) p[0] = 1;
        if (which == 3) p[9] = '=';
        if (which == 4) p[4] = 107;
        if (which == 5) { p[m.len] = 1; ++m.len; }
        if (which == 6) m.compat_flags = 1;
        size_t length = frame(&m, altered, 147, 1, 243);
        if (which >= 6) { // finalize resets flags; set and recompute manually
            altered[which == 6 ? 3 : 2] = 1; uint16_t crc = 0xffff;
            for (size_t i = 1; i < length - 2; ++i) crc = droneid_fc_crc(crc, altered[i]);
            crc = droneid_fc_crc(crc, 147); altered[length-2] = crc; altered[length-1] = crc >> 8;
        }
        assert(!allowed(altered, length));
    }
    droneid_fc_filter_t s = {0};
    assert(feed(&s, good, n, 0) == 1);
    assert(feed(&s, good, n, 199) == 0);
    assert(feed(&s, good, n, 200) == 1);
    assert(feed(&s, health, hn, 200) == 0); // Independent aggregate output budget.
    assert(feed(&s, health, hn, 400) == 1);
    assert(feed(&s, health, hn, 1000) == 0);
    assert(feed(&s, health, hn, 2400) == 1);
    // Valid mixed-frame saturation stays within both per-kind and aggregate
    // output ceilings, independently of the radio and external guard budgets.
    memset(&s, 0, sizeof(s));
    for (unsigned time = 0; time < 10000; time += 100) {
        (void)feed(&s, good, n, time);
        (void)feed(&s, health, hn, time);
    }
    assert(s.accepted[1] <= 50 && s.accepted[2] <= 5 && s.accepted[1] > 20);
    assert(s.accepted[1] * 125 + s.accepted[2] * 133 <= 160 + 800 * 10);
    memset(&s, 0, sizeof(s));
    assert(!feed(&s, good, 60, 0));
    assert(!feed(&s, good + 60, n - 60, 51)); // Partial-frame timeout.
    assert(feed(&s, good, n, 1000) == 1);
    memset(&s, 0, sizeof(s));
    for (size_t i = 0; i < n; ++i) assert(droneid_fc_feed(&s, good[i], i * 2) == 0);
    assert(feed(&s, good, n, 1000) == 1); // Total frame timeout, then recovery.
    memset(&s, 0, sizeof(s));
    assert(feed(&s, good, n, UINT32_MAX - 100) == 1);
    assert(feed(&s, good, n, 100) == 1); // Clock wrap.
    // Arbitrary and adversarial floods: bounded storage and input credit.
    memset(&s, 0, sizeof(s));
    uint32_t random = 1234567;
    for (unsigned i = 0; i < 1000000; ++i) {
        random = random * 1664525U + 1013904223U;
        assert(droneid_fc_feed(&s, (uint8_t)(random >> 24), i / 1000) == 0);
        assert(s.used < DRONEID_FC_FRAME_MAX);
    }
    assert(s.dropped_bytes > 990000);
    assert(feed(&s, good, n, 3000) == 1);
    // Real independent guard output is interoperable with FC policy.
    uart_guard_t guard; guard_config_t config; guard_config_default(&config);
    assert(guard_init(&guard, &config, 0));
    size_t produced = guard_poll(&guard, GUARD_FC, 0, altered);
    assert(produced == 21 && allowed(altered, produced));
    guard.sensor[0].present = true; guard.sensor[0].length = 78;
    memcpy(guard.sensor[0].data, "DRID", 4); guard.sensor[0].data[4] = 1; guard.sensor[0].data[5] = 1;
    guard.sensor[0].data[52] = 1; // Valid BLE observation.
    produced = guard_poll(&guard, GUARD_FC, 100000, altered);
    assert(produced == 125 && allowed(altered, produced));
    s.input_bytes = UINT32_MAX;
    (void)droneid_fc_feed(&s, 0, 3000);
    assert(s.input_bytes == UINT32_MAX);
    puts("FC ingress: canonical guard frames, common-dialect deny policy, schema, corruption, resync, flood, budgets and clock-wrap tests passed");
    return 0;
}
