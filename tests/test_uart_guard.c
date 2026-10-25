#include "uart_guard.h"
#include "guard_selftest.h"
#include "fc_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t receiver[6] = {0x10,0x20,0x30,0x40,0x50,0x60};
static const uint64_t start = 1000000;

static void init(uart_guard_t *g)
{
    guard_config_t config;
    guard_config_default(&config);
    assert(guard_init(g, &config, start));
}

static void feed(uart_guard_t *g, guard_port_t side, const uint8_t *data, size_t size, uint64_t now)
{
    // Deliberately fragmented, with no serial or hardware involved.
    for (size_t at=0; at<size;) {
        size_t n = size-at > 7 ? 7 : size-at;
        assert(guard_ingest(g, side, data+at, n, now) == n);
        at += n;
    }
}

static size_t sensor_observation(uint8_t *wire, const uint8_t raw[25])
{
    fc_link_t link;
    fc_config_t config;
    fc_config_default(&config);
    fc_link_init(&link, &config, receiver, 123);
    rid_observation_t obs = {.seq=7, .rx_us=100, .rssi=-51,
        .transport=RID_WIFI_BEACON, .channel=6, .pack_size=1};
    memcpy(obs.address, receiver, 6);
    memcpy(obs.payload.message, raw, 25);
    return fc_observation(&link, &obs, 101, wire);
}

static size_t generic(uint32_t id, uint8_t system, uint8_t component, uint8_t *wire)
{
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(id);
    assert(entry);
    mavlink_message_t msg = {.msgid=id};
    mavlink_status_t tx = {0};
    mavlink_finalize_message_buffer(&msg, system, component, &tx,
        entry->min_msg_len, entry->max_msg_len, entry->crc_extra);
    return mavlink_msg_to_send_buffer(wire, &msg);
}

static unsigned parse(const uint8_t *wire, size_t size, mavlink_message_t *last)
{
    mavlink_message_t buffer = {0};
    mavlink_status_t parser = {0}, status;
    unsigned count = 0;
    for (size_t i=0; i<size; ++i) {
        if (mavlink_frame_char_buffer(&buffer, &parser, wire[i], last, &status) == MAVLINK_FRAMING_OK) ++count;
    }
    return count;
}

static void safe_output(const uint8_t *wire, size_t size)
{
    assert(size > 0 && wire[0] == 0xfd);
    for (size_t i=1; i<size; ++i) assert(wire[i] != 0xfd && wire[i] != 0xfe);
    mavlink_message_t msg;
    assert(parse(wire, size, &msg) == 1);
    assert(msg.sysid == 1 && msg.compid == 243);
    assert(msg.incompat_flags == 0 && msg.compat_flags == 0);
    if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t hb;
        mavlink_msg_heartbeat_decode(&msg, &hb);
        assert(hb.type == MAV_TYPE_ONBOARD_CONTROLLER && hb.autopilot == MAV_AUTOPILOT_INVALID);
        assert(hb.base_mode == 0 && hb.custom_mode == 0);
    } else {
        assert(msg.msgid == MAVLINK_MSG_ID_TUNNEL);
        mavlink_tunnel_t tunnel;
        mavlink_msg_tunnel_decode(&msg, &tunnel);
        assert(tunnel.target_system == 255 && tunnel.target_component == 190 && tunnel.payload_type == 0);
        assert(!memcmp(tunnel.payload, "DRG2", 4));
        assert(tunnel.payload_length == 108 || tunnel.payload_length == 116);
    }
}

static void fixture(const char *kind, const uint8_t *wire, size_t size)
{
    printf("{\"kind\":\"%s\",\"frame_hex\":\"", kind);
    for (size_t i=0; i<size; ++i) printf("%02x", wire[i]);
    puts("\"}");
}

static void test_policy_and_fixtures(void)
{
    assert(guard_selftest());
    uart_guard_t g;
    init(&g);
    uint8_t wire[MAVLINK_MAX_PACKET_LEN], output[MAVLINK_MAX_PACKET_LEN];
    uint8_t raw[25] = {0x32, 0};
    memcpy(raw+2, "Hello guard", 11);
    size_t size = sensor_observation(wire, raw);
    feed(&g, GUARD_SENSOR, wire, size, start);
    assert(g.counters.accepted[GUARD_SENSOR] == 1);
    size = guard_poll(&g, GUARD_FC, start, output);
    safe_output(output, size); fixture("guard_heartbeat", output, size);
    size = guard_poll(&g, GUARD_FC, start, output);
    safe_output(output, size); fixture("guard_observation", output, size);
    assert(guard_poll(&g, GUARD_FC, start, output) == 0);

    fc_config_t config;
    fc_config_default(&config);
    fc_link_t link;
    fc_link_init(&link, &config, receiver, 123);
    uint32_t counters[11] = {1,2,3,4,5,6,UINT32_MAX,8,9,10,11};
    size = fc_health(&link, 2, 200, counters, wire);
    feed(&g, GUARD_SENSOR, wire, size, start+200000);
    size = guard_poll(&g, GUARD_FC, start+200000, output);
    safe_output(output, size); fixture("guard_health", output, size);

    // A checksum-valid frame of every other common-dialect type is denied.
    // This covers command, mission, parameter, RC, FTP and future common IDs.
    static const mavlink_msg_entry_t entries[] = MAVLINK_MESSAGE_CRCS;
    for (size_t i=0; i<sizeof(entries)/sizeof(entries[0]); ++i) {
        if (entries[i].msgid == 0 || entries[i].msgid == 385) continue;
        init(&g);
        size = generic(entries[i].msgid, 1, 242, wire);
        feed(&g, GUARD_SENSOR, wire, size, start);
        assert(g.counters.accepted[GUARD_SENSOR] == 0);
        assert(!g.sensor[0].present && !g.sensor[1].present);
        size = guard_poll(&g, GUARD_FC, start, output);
        safe_output(output, size);
        mavlink_message_t msg;
        assert(parse(output, size, &msg)==1 && msg.msgid==0);
        assert(guard_poll(&g, GUARD_FC, start, output)==0);
    }
}

static size_t repack(mavlink_tunnel_t *tunnel, uint8_t system, uint8_t component, uint8_t *wire)
{
    mavlink_message_t msg;
    mavlink_status_t status = {0};
    mavlink_msg_tunnel_pack_status(system, component, &status, &msg,
        tunnel->target_system, tunnel->target_component, tunnel->payload_type, tunnel->payload_length, tunnel->payload);
    return mavlink_msg_to_send_buffer(wire, &msg);
}

static void test_rejection_and_recovery(void)
{
    uint8_t valid[MAVLINK_MAX_PACKET_LEN], wire[MAVLINK_MAX_PACKET_LEN], raw[25] = {0x32};
    size_t valid_size = sensor_observation(valid, raw);
    mavlink_message_t msg;
    assert(parse(valid, valid_size, &msg)==1);
    mavlink_tunnel_t original;
    mavlink_msg_tunnel_decode(&msg, &original);
    for (unsigned mutation=0; mutation<16; ++mutation) {
        uart_guard_t g;
        init(&g);
        mavlink_tunnel_t t = original;
        uint8_t sys=1, comp=242;
        switch (mutation) {
            case 0: t.payload_type=1; break;
            case 1: t.target_system=0; break;
            case 2: t.target_component=0; break;
            case 3: sys=2; break;
            case 4: comp=1; break;
            case 5: t.payload[0]='X'; break;
            case 6: t.payload[4]=2; break;
            case 7: t.payload[5]=3; break;
            case 8: t.payload_length=77; break;
            case 9: t.payload[24]=102; break;
            case 10: t.payload[40]=3; break;
            case 11: t.payload[41]=127; break;
            case 12: t.payload[48]=0; break;
            case 13: t.payload[49]=1; break;
            case 14: t.payload[51]=t.payload[52]; break;
            case 15: t.payload[127]=1; break;
        }
        size_t size = repack(&t, sys, comp, wire);
        feed(&g, GUARD_SENSOR, wire, size, start);
        assert(g.counters.accepted[0]==0 && !g.sensor[0].present);
        feed(&g, GUARD_SENSOR, valid, valid_size, start+200000);
        assert(g.sensor[0].present);
    }
    // Bad CRC, truncation, UART fault, timeout, and unsupported flags.
    for (size_t n=0; n<valid_size; ++n) {
        uart_guard_t g;
        init(&g);
        feed(&g, GUARD_SENSOR, valid, n, start);
        assert(!g.sensor[0].present);
        guard_input_fault(&g, GUARD_SENSOR);
        feed(&g, GUARD_SENSOR, valid, valid_size, start+200000);
        assert(g.sensor[0].present);
    }
    uart_guard_t g;
    init(&g);
    memcpy(wire, valid, valid_size); wire[valid_size-1] ^= 1;
    feed(&g, GUARD_SENSOR, wire, valid_size, start);
    assert(!g.sensor[0].present && g.counters.framing_errors[0]);
    feed(&g, GUARD_SENSOR, valid, 8, start+200000);
    feed(&g, GUARD_SENSOR, valid, valid_size, start+400000);
    assert(g.sensor[0].present);
    for (unsigned mode=0; mode<3; ++mode) {
        init(&g);
        memcpy(wire, valid, valid_size);
        wire[mode == 0 ? 3 : 2] = mode == 2 ? 0x80 : 1;
        uint16_t crc = crc_calculate(wire+1, valid_size-3);
        crc_accumulate(MAVLINK_MSG_ID_TUNNEL_CRC, &crc);
        wire[valid_size-2]=(uint8_t)crc; wire[valid_size-1]=(uint8_t)(crc>>8);
        size_t size = valid_size;
        if (mode==1) { memset(wire+size, 0, 13); size+=13; }
        feed(&g, GUARD_SENSOR, wire, size, start);
        assert(g.counters.accepted[0]==0 && !g.sensor[0].present);
    }
    guard_config_t config;
    guard_config_default(&config);
    config.target_system=0;
    assert(!guard_init(&g, &config, start));
    assert(guard_poll(&g, GUARD_FC, start, wire)==0);

    init(&g);
    mavlink_status_t legacy={.flags=MAVLINK_STATUS_FLAG_OUT_MAVLINK1};
    mavlink_message_t heartbeat;
    mavlink_msg_heartbeat_pack_status(1,242,&legacy,&heartbeat,
        MAV_TYPE_ONBOARD_CONTROLLER,MAV_AUTOPILOT_INVALID,0,0,MAV_STATE_ACTIVE);
    size_t legacy_size=mavlink_msg_to_send_buffer(wire,&heartbeat);
    assert(wire[0]==0xfe);
    feed(&g,GUARD_SENSOR,wire,legacy_size,start);
    assert(g.counters.accepted[GUARD_SENSOR]==0 && g.counters.rejected[GUARD_SENSOR]==1);
}

static void test_nested_frame_and_restart(void)
{
    uart_guard_t g;
    init(&g);
    uint8_t wire[MAVLINK_MAX_PACKET_LEN], raw[25]={0}, nested[MAVLINK_MAX_PACKET_LEN], output[MAVLINK_MAX_PACKET_LEN];
    mavlink_message_t hb;
    mavlink_status_t status={0};
    mavlink_msg_heartbeat_pack_status(99,99,&status,&hb,MAV_TYPE_ONBOARD_CONTROLLER,MAV_AUTOPILOT_INVALID,0,0,MAV_STATE_ACTIVE);
    size_t n=mavlink_msg_to_send_buffer(nested,&hb);
    assert(n<=sizeof(raw)); memcpy(raw,nested,n);
    size_t size=sensor_observation(wire,raw);
    // An unguarded opaque binary payload contains a valid nested frame if a
    // consumer loses the enclosing frame boundary. No real command is sent.
    mavlink_message_t decoded;
    assert(parse(wire+68,size-68,&decoded)==1 && decoded.sysid==99);
    feed(&g,GUARD_SENSOR,wire,size,start);
    assert(guard_poll(&g,GUARD_FC,start,output)>0); // Own heartbeat first.
    n=guard_poll(&g,GUARD_FC,start,output);
    safe_output(output,n);
    for (size_t offset=1;offset<n;++offset) {
        assert(parse(output+offset,n-offset,&decoded)==0);
        uint8_t joined[2*MAVLINK_MAX_PACKET_LEN];
        memcpy(joined,output+offset,n-offset);memcpy(joined+n-offset,output,n);
        assert(parse(joined,2*n-offset,&decoded)==1);
        assert(decoded.msgid==MAVLINK_MSG_ID_TUNNEL && decoded.compid==243);
    }
    // Sequence wrap and checksum selection must preserve the same invariant
    // for arbitrary record bytes, including both MAVLink start markers.
    uint32_t random=123;
    for (unsigned trial=0;trial<512;++trial) {
        init(&g);
        for (unsigned i=0;i<25;++i) { random=random*1664525u+1013904223u;raw[i]=random>>24; }
        raw[0]=0xfd;raw[1]=0xfe;
        size=sensor_observation(wire,raw);
        feed(&g,GUARD_SENSOR,wire,size,start);
        g.sequence[GUARD_FC]=(uint8_t)trial;
        n=guard_poll(&g,GUARD_FC,start,output);safe_output(output,n);
        n=guard_poll(&g,GUARD_FC,start,output);safe_output(output,n);
        assert(g.counters.encode_errors==0);
    }
}

static size_t controller_heartbeat(uint8_t *wire)
{
    mavlink_message_t msg;
    mavlink_status_t status={0};
    mavlink_msg_heartbeat_pack_status(1,1,&status,&msg,MAV_TYPE_QUADROTOR,MAV_AUTOPILOT_ARDUPILOTMEGA,0,0,MAV_STATE_STANDBY);
    return mavlink_msg_to_send_buffer(wire,&msg);
}

static void test_controller_direction(void)
{
    uart_guard_t g;
    init(&g);
    uint8_t wire[MAVLINK_MAX_PACKET_LEN],out[MAVLINK_MAX_PACKET_LEN];
    size_t size=generic(MAVLINK_MSG_ID_GLOBAL_POSITION_INT,1,1,wire);
    feed(&g,GUARD_FC,wire,size,start);
    assert(guard_poll(&g,GUARD_SENSOR,start,out)==0);
    size=controller_heartbeat(wire);feed(&g,GUARD_FC,wire,size,start);
    size=guard_poll(&g,GUARD_SENSOR,start,out);
    mavlink_message_t msg;
    assert(parse(out,size,&msg)==1 && msg.msgid==0 && msg.sysid==1 && msg.compid==1);
    size=generic(MAVLINK_MSG_ID_GLOBAL_POSITION_INT,1,1,wire);feed(&g,GUARD_FC,wire,size,start+10000);
    size=guard_poll(&g,GUARD_SENSOR,start+10000,out);
    assert(parse(out,size,&msg)==1 && msg.msgid==MAVLINK_MSG_ID_GLOBAL_POSITION_INT);
    size=generic(MAVLINK_MSG_ID_GPS_RAW_INT,1,1,wire);feed(&g,GUARD_FC,wire,size,start+20000);
    size=guard_poll(&g,GUARD_SENSOR,start+20000,out);
    assert(parse(out,size,&msg)==1 && msg.msgid==MAVLINK_MSG_ID_GPS_RAW_INT);
    size=generic(MAVLINK_MSG_ID_COMMAND_LONG,1,1,wire);feed(&g,GUARD_FC,wire,size,start+30000);
    assert(guard_poll(&g,GUARD_SENSOR,start+30000,out)==0);
    guard_input_fault(&g,GUARD_FC);
    size=generic(MAVLINK_MSG_ID_GLOBAL_POSITION_INT,1,1,wire);feed(&g,GUARD_FC,wire,size,start+6000000);
    assert(guard_poll(&g,GUARD_SENSOR,start+6000000,out)==0);

    static const mavlink_msg_entry_t entries[] = MAVLINK_MESSAGE_CRCS;
    for (size_t i=0;i<sizeof(entries)/sizeof(entries[0]);++i) {
        if (entries[i].msgid==0 || entries[i].msgid==24 || entries[i].msgid==33) continue;
        init(&g);
        size=generic(entries[i].msgid,1,1,wire);
        feed(&g,GUARD_FC,wire,size,start);
        assert(guard_poll(&g,GUARD_SENSOR,start,out)==0);
        assert(g.counters.accepted[GUARD_FC]==0);
    }
}

static void test_budgets_and_noise(void)
{
    uart_guard_t g;
    init(&g);
    uint8_t wire[MAVLINK_MAX_PACKET_LEN], out[MAVLINK_MAX_PACKET_LEN], raw[25]={0x32};
    size_t size=sensor_observation(wire,raw);
    uint64_t last_obs=0,last_hb=0;
    unsigned observations=0;
    for (unsigned step=0;step<1000;++step) {
        uint64_t now=start+(uint64_t)step*10000;
        guard_ingest(&g,GUARD_SENSOR,wire,size,now); // Flood includes discarded input.
        size_t n=guard_poll(&g,GUARD_FC,now,out);
        if (!n) continue;
        safe_output(out,n);
        mavlink_message_t msg;assert(parse(out,n,&msg)==1);
        if (msg.msgid==0) { if(last_hb)assert(now-last_hb>=1000000);last_hb=now; }
        else { if(last_obs)assert(now-last_obs>=200000);last_obs=now;++observations; }
    }
    assert(observations>0 && observations<=50);
    assert(g.counters.output_bytes[GUARD_FC] <= GUARD_OUTPUT_BURST+10*GUARD_FC_OUTPUT_RATE);
    assert(g.counters.input_dropped[GUARD_SENSOR]>0);
    feed(&g,GUARD_SENSOR,wire,size,start+12000000);
    assert(g.sensor[0].present);
    guard_poll(&g,GUARD_FC,start+14000000,out);
    assert(!g.sensor[0].present && g.counters.expired[0]>0);

    uint32_t seed=42;
    uint8_t noise[512];
    for(unsigned trial=0;trial<10000;++trial) {
        for(size_t i=0;i<sizeof(noise);++i) { seed=seed*1664525u+1013904223u;noise[i]=seed>>24; }
        uint64_t now=start+15000000+(uint64_t)trial*10000;
        assert(guard_ingest(&g,GUARD_SENSOR,noise,sizeof(noise),now)<=GUARD_INPUT_CHUNK);
        assert(guard_ingest(&g,GUARD_FC,noise,sizeof(noise),now)<=GUARD_INPUT_CHUNK);
        size_t n=guard_poll(&g,GUARD_FC,now,out);
        if(n)safe_output(out,n);
        guard_poll(&g,GUARD_SENSOR,now,out);
    }
    feed(&g,GUARD_SENSOR,wire,size,start+120000000);
    assert(g.sensor[0].present);
}

int main(void)
{
    test_policy_and_fixtures();
    test_rejection_and_recovery();
    test_nested_frame_and_restart();
    test_controller_direction();
    test_budgets_and_noise();
    fprintf(stderr,"Independent UART guard: allowlist, schema, restart-safe output, bidirectional filtering, budgets and malformed-stream trials passed.\n");
}
