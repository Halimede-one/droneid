#include "guard_selftest.h"
#include <string.h>

bool guard_selftest(void)
{
    uart_guard_t g;
    guard_config_t config;
    guard_config_default(&config);
    if (!guard_init(&g, &config, 1000000)) return false;
    mavlink_message_t m;
    mavlink_status_t tx = {0};
    uint8_t wire[MAVLINK_MAX_PACKET_LEN], out[MAVLINK_MAX_PACKET_LEN];
    // A harmless but forbidden request must be rejected, even with valid CRC.
    mavlink_msg_param_request_list_pack_status(1, 242, &tx, &m, 1, 1);
    size_t n = mavlink_msg_to_send_buffer(wire, &m);
    if (guard_ingest(&g, GUARD_SENSOR, wire, n, 1000000) != n || g.counters.rejected[0] != 1) return false;
    // A valid observation must be wrapped; no nested frame markers may escape.
    uint8_t p[128] = {'D','R','I','D',1,1};
    p[41] = (uint8_t)-50; p[52] = 1; p[53] = 0x32;
    mavlink_msg_tunnel_pack_status(1, 242, &tx, &m, 255, 190, 0, 78, p);
    n = mavlink_msg_to_send_buffer(wire, &m);
    if (guard_ingest(&g, GUARD_SENSOR, wire, n, 1000000) != n || g.counters.accepted[0] != 1) return false;
    for (unsigned i=0; i<2; ++i) {
        n = guard_poll(&g, GUARD_FC, 1000000, out);
        if (!n || out[0] != MAVLINK_STX) return false;
        for (size_t j=1; j<n; ++j) if (out[j] == 0xfd || out[j] == 0xfe) return false;
        if (out[7] != (i ? (385 & 255) : 0)) return false;
    }
    if (guard_poll(&g, GUARD_FC, 1000000, out)) return false;
    // FC heartbeat is regenerated on the sensor link, commands are not.
    mavlink_msg_heartbeat_pack_status(1, 1, &tx, &m, MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
    n = mavlink_msg_to_send_buffer(wire, &m);
    if (guard_ingest(&g, GUARD_FC, wire, n, 1000000) != n || !guard_poll(&g, GUARD_SENSOR, 1000000, out)) return false;
    return g.counters.accepted[1] == 1;
}
