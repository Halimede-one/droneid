/* Offline boundary probes: no radios, serial ports, or flight controller. */
#include "fc_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static mavlink_message_t single_frame(const uint8_t *bytes, size_t size)
{
    mavlink_message_t buffer = {0}, received = {0}, result = {0};
    mavlink_status_t parser = {0}, status = {0};
    unsigned frames = 0;
    for (size_t i = 0; i < size; ++i) {
        uint8_t rc = mavlink_frame_char_buffer(&buffer, &parser, bytes[i], &received, &status);
        assert(rc != MAVLINK_FRAMING_BAD_CRC && rc != MAVLINK_FRAMING_BAD_SIGNATURE);
        if (rc == MAVLINK_FRAMING_OK) { result = received; ++frames; }
    }
    assert(frames == 1);
    return result;
}

int main(void)
{
    fc_config_t config;
    fc_config_default(&config);
    const uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    fc_link_t link;
    fc_link_init(&link, &config, mac, 1);

    // A benign, complete MAVLink heartbeat inside the opaque radio bytes must
    // remain data. It must not become a second, top-level UART message.
    mavlink_message_t heartbeat;
    mavlink_status_t sender = {0};
    mavlink_msg_heartbeat_pack_status(9, 9, &sender, &heartbeat,
        MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    uint8_t nested[MAVLINK_MAX_PACKET_LEN], wire[MAVLINK_MAX_PACKET_LEN];
    size_t nested_size = mavlink_msg_to_send_buffer(nested, &heartbeat);
    rid_observation_t observation = {.transport = RID_WIFI_BEACON, .rssi = -50,
        .rx_us = 100, .seq = 1, .channel = 6, .pack_size = 1};
    assert(nested_size <= sizeof(observation.payload.message));
    memcpy(observation.payload.message, nested, nested_size);
    size_t size = fc_observation(&link, &observation, 200, wire);
    mavlink_message_t frame = single_frame(wire, size);
    assert(frame.msgid == MAVLINK_MSG_ID_TUNNEL);
    assert(frame.sysid == config.system_id && frame.compid == config.component_id);
    mavlink_tunnel_t tunnel;
    mavlink_msg_tunnel_decode(&frame, &tunnel);
    assert(tunnel.target_system == config.target_system);
    assert(tunnel.target_component == config.target_component);
    assert(tunnel.payload_type == 0 && tunnel.payload_length == FC_OBSERVATION_SIZE);
    assert(memcmp(tunnel.payload + 53, observation.payload.message, 25) == 0);
    puts("{\"check\":\"nested_mavlink_remains_opaque\",\"result\":\"observed\",\"outer_frames\":1,\"outer_message_id\":385,\"raw_bytes_preserved\":true}");

    // Matching IDs and a CRC suffice for current telemetry input. Replay the
    // same unsigned heartbeat after expiry; do not construct any command.
    sender = (mavlink_status_t){0};
    mavlink_msg_heartbeat_pack_status(config.system_id, config.fc_component_id,
        &sender, &heartbeat, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        0, 0, MAV_STATE_STANDBY);
    size = mavlink_msg_to_send_buffer(wire, &heartbeat);
    assert(!(heartbeat.incompat_flags & MAVLINK_IFLAG_SIGNED));
    fc_receive(&link, wire, size, 1000);
    assert(fc_heartbeat_fresh(&link.telemetry, 1000));
    uint64_t later = 1000 + FC_STALE_US + 1;
    assert(!fc_heartbeat_fresh(&link.telemetry, later));
    fc_receive(&link, wire, size, later);
    assert(fc_heartbeat_fresh(&link.telemetry, later));
    assert(link.telemetry.rx_frames == 2);
    puts("{\"check\":\"unsigned_heartbeat_replay\",\"result\":\"observed\",\"accepted_without_key\":true,\"identical_replay_refreshes_age\":true}");
    return 0;
}
