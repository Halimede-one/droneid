#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rid.h"
#include "wifi_fixtures.h"

static const uint8_t basic_fixture[31] = {
    30, 0x16, 0xfa, 0xff, 0x0d, 7,
    0x02, 0x12, 'T', 'E', 'S', 'T', '-', 'D', 'R', 'O', 'N', 'E',
};

static void output(rid_observation_t *o)
{
    char json[RID_JSON_SIZE];
    size_t len = rid_observation_json(json, sizeof(json), o, "102030405060", "session1", 2000);
    assert(len > 0 && len == strlen(json));
    assert(json[len - 1] == '\n');
    fputs(json, stdout);
    // Every insufficient capacity must fail without returning a partial record.
    for (size_t size = 0; size <= len; ++size) {
        char bounded[RID_JSON_SIZE];
        memset(bounded, 0xa5, sizeof(bounded));
        assert(rid_observation_json(bounded, size, o, "102030405060", "session1", 2000) == 0);
        assert((unsigned char)bounded[size] == 0xa5);
    }
}

static void test_wifi(void)
{
    rid_wifi_packet_t packet;
    assert(rid_parse_wifi(wifi_beacon_fixture, sizeof(wifi_beacon_fixture), &packet) == 1);
    assert(packet.transport == RID_WIFI_BEACON && packet.count == 2);
    assert(packet.messages[0].counter == 7 && packet.messages[1].counter == 7);
    assert(!memcmp(packet.messages[0].message, basic_fixture + 6, ODID_MESSAGE_SIZE));
    assert(packet.messages[1].message[0] == 0x32);
    const uint8_t mac[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    assert(!memcmp(packet.address, mac, 6));
    rid_observation_t o = {.transport = RID_WIFI_BEACON, .channel = 6,
        .rssi = -43, .rx_us = 1000, .seq = 43, .pack_size = 2};
    memcpy(o.address, packet.address, 6);
    o.payload = packet.messages[0];
    output(&o);
    assert(rid_parse_wifi(wifi_nan_fixture, sizeof(wifi_nan_fixture), &packet) == 1);
    assert(packet.transport == RID_WIFI_NAN && packet.count == 2);
    assert(packet.messages[0].counter == 9);
    o.transport = RID_WIFI_NAN; o.channel = 149; o.pack_index = 1;
    o.payload = packet.messages[1];
    output(&o);
    rid_wifi_packet_t untouched;
    memset(&untouched, 0xa5, sizeof(untouched));
    for (size_t len = 0; len < sizeof(wifi_beacon_fixture); ++len) {
        packet = untouched;
        assert(rid_parse_wifi(wifi_beacon_fixture, len, &packet) != 1);
        assert(!memcmp(&packet, &untouched, sizeof(packet)));
    }
    for (size_t len = 0; len < sizeof(wifi_nan_fixture); ++len) {
        int rc = rid_parse_wifi(wifi_nan_fixture, len, &packet);
        // The final extension is optional; the complete descriptor ends at 97.
        assert(rc != 1 || len == 97);
    }
    uint8_t frame[512];
    const size_t fields[] = {31, 32, 42, 44, 45, 46};
    for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
        for (unsigned value = 0; value <= 255; ++value) {
            memcpy(frame, wifi_nan_fixture, sizeof(wifi_nan_fixture));
            frame[fields[f]] = value;
            int rc = rid_parse_wifi(frame, sizeof(wifi_nan_fixture), &packet);
            assert(rc != 1 || value == wifi_nan_fixture[fields[f]] ||
                   (fields[f] == 44 && value >= 0xf0 && value <= 0xf2));
        }
    }
    memcpy(frame, wifi_beacon_fixture, sizeof(wifi_beacon_fixture));
    frame[58] ^= 1; // Wrong OUI, unrelated vendor data.
    assert(rid_parse_wifi(frame, sizeof(wifi_beacon_fixture), &packet) == 0);
    memcpy(frame, wifi_beacon_fixture, sizeof(wifi_beacon_fixture));
    memcpy(frame + sizeof(wifi_beacon_fixture), frame + 54, 60); // Duplicate RID IE.
    assert(rid_parse_wifi(frame, sizeof(wifi_beacon_fixture) + 60, &packet) == -1);
    // Unknown IEs/attributes preceding the RID data must not hide valid packets.
    memcpy(frame, wifi_nan_fixture, 30);
    frame[30] = 0x7f; frame[31] = 1; frame[32] = 0; frame[33] = 0;
    memcpy(frame + 34, wifi_nan_fixture + 30, sizeof(wifi_nan_fixture) - 30);
    assert(rid_parse_wifi(frame, sizeof(wifi_nan_fixture) + 4, &packet) == 1);
    size_t len = rid_bench_wifi_beacon(mac, mac, 255, 12345, frame, sizeof(frame));
    assert(len > 0 && len <= RID_WIFI_FRAME_SIZE);
    assert(rid_parse_wifi(frame, len, &packet) == 1 && packet.count == 2);
    assert(packet.messages[0].counter == 255 && packet.messages[0].message[2] == 0xff);
    assert(frame[24] == 0x39 && frame[25] == 0x30);
    for (size_t cap = 0; cap < len; ++cap) {
        uint8_t buffer[RID_WIFI_FRAME_SIZE];
        memset(buffer, 0xa5, sizeof(buffer));
        assert(rid_bench_wifi_beacon(mac, mac, 0, 0, buffer, cap) == 0);
        for (size_t i = 0; i < sizeof(buffer); ++i) assert(buffer[i] == 0xa5);
    }
    // Exercise both random bytes and structurally plausible mutated frames.
    uint32_t seed = 0x87654321;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        for (size_t i = 0; i < sizeof(frame); ++i) {
            seed = seed * 1664525u + 1013904223u;
            frame[i] = seed >> 24;
        }
        rid_parse_wifi(frame, trial % sizeof(frame), &packet);
        memcpy(frame, trial % 2 ? wifi_nan_fixture : wifi_beacon_fixture, 24);
        rid_parse_wifi(frame, trial % sizeof(frame), &packet);
    }
}

static void test_identity(void)
{
    const uint8_t mac[] = {0x10,0x20,0x30,0x40,0x50,0x60};
    rid_identity_t identity;
    rid_identity_default(&identity, mac);
    assert(rid_identity_valid(&identity));
    assert(!strcmp(identity.id, "BENCH-102030405060"));
    assert(rid_identity_from_hex(&identity, "53434f55542d3031", "48692022555342225c"));
    assert(!strcmp(identity.id,"SCOUT-01") && !strcmp(identity.hello,"Hi \"USB\"\\"));
    uint8_t adv[RID_ADV_SIZE], frame[RID_WIFI_FRAME_SIZE];
    rid_payload_t basic, self;
    assert(rid_identity_advertisement(&identity,false,1,adv));
    assert(rid_parse_legacy(adv,sizeof(adv),&basic)==1);
    assert(basic.message[2]==0xff && !memcmp(basic.message+3,"SCOUT-01",8));
    assert(rid_identity_advertisement(&identity,true,2,adv));
    assert(rid_parse_legacy(adv,sizeof(adv),&self)==1);
    ODID_SelfID_data decoded;
    assert(decodeSelfIDMessage(&decoded,(const ODID_SelfID_encoded *)self.message)==ODID_SUCCESS);
    assert(!strcmp(decoded.Desc,identity.hello));
    size_t n=rid_identity_wifi_beacon(&identity,mac,3,0,frame,sizeof(frame));
    rid_wifi_packet_t packet;
    assert(n && rid_parse_wifi(frame,n,&packet)==1);
    assert(!memcmp(basic.message,packet.messages[0].message,25));
    assert(!memcmp(self.message,packet.messages[1].message,25));
    const rid_identity_t saved=identity;
    const char *bad[] = {"", "00", "0", "GG", "41420043", "3c7376673e", "414141414141414141414141414141414141414141"};
    for (size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        assert(!rid_identity_from_hex(&identity,bad[i],"-"));
        assert(!memcmp(&identity,&saved,sizeof(saved)));
    }
    assert(!rid_identity_from_hex(&identity,"41","0a"));
    assert(!rid_identity_from_hex(&identity,"41","7f"));
    assert(!rid_identity_from_hex(&identity,"41","c3a9"));
    assert(rid_identity_from_hex(&identity,"41","-"));
    assert(!strcmp(identity.id,"A") && !identity.hello[0]);
    memset(identity.id,'A',RID_BENCH_ID_MAX);identity.id[RID_BENCH_ID_MAX]=0;
    memset(identity.hello,'!',RID_HELLO_MAX);identity.hello[RID_HELLO_MAX]=0;
    assert(rid_identity_valid(&identity));
    assert(rid_identity_advertisement(&identity,false,0,adv));
    assert(adv[27]=='A');
    identity.id[RID_BENCH_ID_MAX]='A';
    assert(!rid_identity_valid(&identity));
}

int main(void)
{
    rid_payload_t payload;
    assert(rid_parse_legacy(basic_fixture, sizeof(basic_fixture), &payload) == 1);
    assert(payload.counter == 7 && payload.message[0] == 2);
    assert(!memcmp(payload.message + 2, "TEST-DRONE", 10));
    for (size_t len = 1; len < sizeof(basic_fixture); ++len)
        assert(rid_parse_legacy(basic_fixture, len, &payload) == -1);
    assert(rid_parse_legacy(basic_fixture, 0, &payload) == 0);
    assert(rid_parse_legacy(NULL, 0, &payload) == -1);
    uint8_t adv[RID_ADV_SIZE];
    memcpy(adv, basic_fixture, sizeof(adv));
    adv[4] = 0x0e;
    assert(rid_parse_legacy(adv, sizeof(adv), &payload) == 0);
    const uint8_t flags[] = {2, 1, 6};
    assert(rid_parse_legacy(flags, sizeof(flags), &payload) == 0);
    uint8_t truncated_tail[32];
    memcpy(truncated_tail, basic_fixture, sizeof(basic_fixture));
    truncated_tail[31] = 5;
    assert(rid_parse_legacy(truncated_tail, sizeof(truncated_tail), &payload) == -1);

    rid_observation_t o = {.rssi = -57, .rx_us = 1000, .seq = 42,
                           .address = {1, 2, 3, 4, 5, 6}};
    assert(rid_parse_legacy(basic_fixture, sizeof(basic_fixture), &o.payload) == 1);
    output(&o);
    const uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    assert(rid_bench_advertisement(mac, false, 255, adv));
    assert(!memcmp(adv, basic_fixture, 5));
    assert(adv[5] == 255 && adv[6] == 2 && adv[7] == 0x40 && adv[8] == 0xff);
    assert(!memcmp(adv + 9, "BENCH-102030405060", 18));
    assert(rid_parse_legacy(adv, sizeof(adv), &o.payload) == 1);
    output(&o);
    assert(rid_bench_advertisement(mac, true, 0, adv));
    assert(rid_parse_legacy(adv, sizeof(adv), &o.payload) == 1);
    assert(o.payload.message[0] == 0x32);
    output(&o);

    ODID_Location_data location;
    ODID_Message_encoded encoded;
    odid_initLocationData(&location);
    location.TimeStamp = INV_TIMESTAMP;
    assert(encodeLocationMessage(&encoded.location, &location) == ODID_SUCCESS);
    memcpy(o.payload.message, encoded.rawData, ODID_MESSAGE_SIZE);
    o.rssi = 127;
    output(&o);
    location.Latitude = 37.5;
    location.Longitude = -122.25;
    location.AltitudeGeo = 12.5;
    location.TimeStamp = 123.4;
    assert(encodeLocationMessage(&encoded.location, &location) == ODID_SUCCESS);
    memcpy(o.payload.message, encoded.rawData, ODID_MESSAGE_SIZE);
    output(&o);
    o.payload.message[0] = 0x13; // Unknown future protocol: preserve raw only.
    output(&o);

    ODID_SelfID_data self;
    odid_initSelfIDData(&self);
    memcpy(self.Desc, "quote\" slash\\ newline\n", 22);
    assert(encodeSelfIDMessage(&encoded.selfId, &self) == ODID_SUCCESS);
    memcpy(o.payload.message, encoded.rawData, ODID_MESSAGE_SIZE);
    output(&o);
    ODID_System_data system;
    odid_initSystemData(&system);
    assert(encodeSystemMessage(&encoded.system, &system) == ODID_SUCCESS);
    memcpy(o.payload.message, encoded.rawData, ODID_MESSAGE_SIZE);
    output(&o);

    // Exercise arbitrary on-air bytes through framing and all message decoders.
    uint32_t seed = 0x12345678;
    char json[RID_JSON_SIZE];
    for (unsigned trial = 0; trial < 10000; ++trial) {
        for (size_t i = 0; i < sizeof(adv); ++i) {
            seed = seed * 1664525u + 1013904223u;
            adv[i] = seed >> 24;
        }
        rid_parse_legacy(adv, trial % (sizeof(adv) + 1), &payload);
        memcpy(o.payload.message, adv, ODID_MESSAGE_SIZE);
        assert(rid_observation_json(json, sizeof(json), &o, "receiver", "boot", 2000) > 0);
    }
    test_wifi();
    test_identity();
    fprintf(stderr, "BLE/Wi-Fi framing, JSON bounds, upstream fixtures and malformed-input trials passed.\n");
    return 0;
}
