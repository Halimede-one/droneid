#include "droneid_fc_filter.h"
#include "fc_direct_fixture.h"
#include <stdio.h>

static unsigned ingest(droneid_fc_filter_t *s, const uint8_t *bytes, size_t n, uint32_t now)
{
    unsigned result = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned length = droneid_fc_feed(s, bytes[i], now);
        if (length) { assert(result == 0); result = length; }
    }
    return result;
}

static bool accepts(const uint8_t *bytes, size_t n)
{
    droneid_fc_filter_t filter = {0};
    return ingest(&filter, bytes, n, 0) != 0;
}

static void fixture(const uint8_t *input, size_t n, unsigned kind, unsigned wanted)
{
    droneid_fc_filter_t filter = {0};
    unsigned length = ingest(&filter, input, n, 0);
    assert(length == wanted && filter.accepted[kind] == 1);
    assert(filter.frame[6] == 242);
    for (unsigned i = 1; i < length; ++i) assert(filter.frame[i] != 0xfd && filter.frame[i] != 0xfe);
    // No native parser restart inside the normalized output can find a frame.
    for (unsigned offset = 1; offset < length; ++offset) {
        mavlink_message_t parser = {0}, msg;
        mavlink_status_t status = {0}, out;
        for (unsigned i = offset; i < length; ++i)
            assert(mavlink_frame_char_buffer(&parser, &status, filter.frame[i], &msg, &out) != MAVLINK_FRAMING_OK);
    }
    printf("{\"input_hex\":\"");
    for (size_t i = 0; i < n; ++i) printf("%02x", input[i]);
    printf("\",\"frame_hex\":\"");
    for (unsigned i = 0; i < length; ++i) printf("%02x", filter.frame[i]);
    puts("\"}");
}

static void checksum(uint8_t *frame, unsigned size, uint8_t extra)
{
    uint16_t crc = 0xffff;
    for (unsigned i = 1; i < size - 2; ++i) crc = droneid_fc_crc(crc, frame[i]);
    crc = droneid_fc_crc(crc, extra);
    frame[size - 2] = crc; frame[size - 1] = crc >> 8;
}

int main(void)
{
    uint8_t input[MAVLINK_MAX_PACKET_LEN], bad[MAVLINK_MAX_PACKET_LEN];
    unsigned n = direct_observation(input, false);
    assert(input[4] == 0xfd && accepts(input, n));
    fixture(input, n, 1, 125);
    for (unsigned offset = 0; offset < n; ++offset) {
        memcpy(bad, input, n); bad[offset] ^= 1;
        assert(!accepts(bad, n));
    }
    // Invalid outer CRC cannot release an embedded command as another packet.
    memcpy(bad, input, n); bad[n - 1] ^= 1;
    assert(!accepts(bad, n));
    for (unsigned offset = 1; offset < n; ++offset) assert(!accepts(input + offset, n - offset));
    for (unsigned seq = 0; seq < 256; ++seq) {
        memcpy(bad, input, n); bad[4] = seq; checksum(bad, n, 147);
        assert(accepts(bad, n)); // Includes start bytes in sequence and CRC.
    }
    // Metadata and peer/encoding restrictions still apply with valid CRCs.
    const unsigned offsets[] = {2, 3, 5, 6, 10, 12, 13, 14, 19, 20, 55, 56, 66, 67};
    const uint8_t values[] = {1, 1, 2, 243, 1, 0, 0, 77, 2, 3, 3, 127, 1, 0};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        memcpy(bad, input, n); bad[offsets[i]] = values[i]; checksum(bad, n, 147);
        assert(!accepts(bad, n));
    }
    // TUNNEL array zero padding is permitted, hidden nonzero suffixes are not.
    memcpy(bad, input, n - 2); bad[1] = 133;
    memset(bad + n - 2, 0, 145 - (n - 2)); checksum(bad, 145, 147);
    assert(accepts(bad, 145));
    bad[143 - 1] = 1; checksum(bad, 145, 147);
    assert(!accepts(bad, 145));
    n = direct_observation(input, true);
    assert(n < 95); // Actual radio's zero-trimmed TUNNEL.
    fixture(input, n, 1, 125);
    // All message classes except the exact heartbeat/report schema are denied
    // even when the attacker claims the direct-radio source component.
    static const mavlink_msg_entry_t entries[] = MAVLINK_MESSAGE_CRCS;
    for (unsigned i = 0; i < sizeof(entries)/sizeof(entries[0]); ++i) {
        const mavlink_msg_entry_t *e = &entries[i];
        if (e->msgid == 0 || e->msgid == 385) continue;
        mavlink_message_t m = {0}; mavlink_status_t tx = {0};
        m.msgid = e->msgid;
        mavlink_finalize_message_buffer(&m, 1, 242, &tx, e->min_msg_len, e->max_msg_len, e->crc_extra);
        assert(!accepts(bad, mavlink_msg_to_send_buffer(bad, &m)));
    }
    fc_config_t config; fc_config_default(&config);
    fc_link_t radio; const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    fc_link_init(&radio, &config, mac, 0xfdfe);
    radio.tx_status.current_tx_seq = 0xfe;
    n = fc_heartbeat(&radio, input); fixture(input, n, 0, 21);
    uint32_t counters[11];
    for (unsigned i = 0; i < 11; ++i) counters[i] = 0xfefdfefd;
    n = fc_health(&radio, 0xfefd, 100, counters, input); fixture(input, n, 2, 133);
    // Direct traffic shares the same independent report budget.
    droneid_fc_filter_t filter = {0};
    n = direct_observation(input, false);
    for (unsigned ms = 0; ms < 10000; ms += 100) (void)ingest(&filter, input, n, ms);
    assert(filter.accepted[1] == 50);
    fprintf(stderr, "FC standalone: real radio DRID, nested command containment, zero trimming, schema, both source IDs, restart-safe normalization and rate limits passed\n");
}
