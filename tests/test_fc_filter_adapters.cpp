#include "droneid_fc_filter.h"
extern "C" {
#include "uart_guard.h"
}
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
#include "fc_direct_fixture.h"

// Exercise the exact included receive adapter with a fake UART and the real
// MAVLink parser, including its signing policy. No simulated command parser.
#define MIN(a, b) std::min((a), (b))
struct Port {
    std::vector<uint8_t> data;
    size_t at = 0;
    uint32_t available() { return data.size() - at; }
    int16_t read() { return at < data.size() ? data[at++] : -1; }
};
static uint32_t now_ms;
namespace AP_HAL { uint32_t millis() { return now_ms; } }
namespace AP {
struct SerialManager {
    Port *ports[9]{};
    Port *get_serial_by_id(unsigned i) { return ports[i]; }
};
static SerialManager serial;
SerialManager &serialmanager() { return serial; }
}
struct GCS_MAVLINK {
    Port *_port = nullptr;
    droneid_fc_filter_t droneid_filter{};
    bool droneid_dispatch = false;
    mavlink_message_t buffer{};
    mavlink_status_t status{};
    unsigned handlers = 0, routed = 0, native_frames = 0;
    uint8_t last_component = 0;
    struct Router {
        bool check_and_forward(GCS_MAVLINK &link, const mavlink_message_t &msg) {
            assert(msg.msgid == MAVLINK_MSG_ID_TUNNEL);
            mavlink_tunnel_t tunnel;
            mavlink_msg_tunnel_decode(&msg, &tunnel);
            assert(memcmp(tunnel.payload, "DRG2", 4) == 0);
            link.last_component = msg.compid;
            ++link.routed;
            return true; // Even a router requesting local handling is ignored.
        }
    } routing;
    bool droneid_uart() const;
    bool route_droneid_packet(const mavlink_message_t &msg);
    void update_droneid_receive();
    mavlink_message_t *channel_buffer() { return &buffer; }
    mavlink_status_t *channel_status() { return &status; }
    virtual void packetReceived(const mavlink_status_t &, const mavlink_message_t &msg) {
        ++native_frames;
        if (route_droneid_packet(msg)) return;
        ++handlers;
    }
};
#include "ardupilot_receive.inc"

struct CopterLink : GCS_MAVLINK {
    unsigned vehicle_handlers = 0;
    void packetReceived(const mavlink_status_t &s, const mavlink_message_t &msg) override {
        ++vehicle_handlers;
        GCS_MAVLINK::packetReceived(s, msg);
    }
};

// iNav uses the same policy but consumes all inbound frames without native
// handlers/routing. Compile and exercise the actual adapter as well.
static Port *mavlinkPort;
static uint32_t millis() { return now_ms; }
static unsigned serialRxBytesWaiting(Port *p) { return p->available(); }
static uint8_t serialRead(Port *p) { return (uint8_t)p->read(); }
#include "inav_receive.inc"

static void drain(GCS_MAVLINK &link)
{
    while (link._port->available()) {
        size_t before = link._port->at;
        link.update_droneid_receive();
        assert(link._port->at - before <= DRONEID_FC_READ_LIMIT);
    }
}

int main()
{
    Port guard_port, gcs_port;
    AP::serial.ports[4] = &guard_port;
    AP::serial.ports[0] = &gcs_port;
    CopterLink protected_link;
    GCS_MAVLINK trusted_link;
    protected_link._port = &guard_port; trusted_link._port = &gcs_port;
    assert(protected_link.droneid_uart() && !trusted_link.droneid_uart());
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN];
    uart_guard_t guard; guard_config_t config; guard_config_default(&config);
    assert(guard_init(&guard, &config, 0));
    size_t n = guard_poll(&guard, GUARD_FC, 0, bytes);
    assert(n == 21);
    guard_port.data.assign(bytes, bytes + n);
    drain(protected_link);
    assert(protected_link.native_frames == 1 && protected_link.routed == 0 && protected_link.handlers == 0);
    guard.sensor[0].present = true; guard.sensor[0].length = 78;
    memcpy(guard.sensor[0].data, "DRID", 4); guard.sensor[0].data[4] = 1; guard.sensor[0].data[5] = 1;
    guard.sensor[0].data[52] = 1;
    n = guard_poll(&guard, GUARD_FC, 100000, bytes);
    assert(n == 125);
    std::vector<uint8_t> report(bytes, bytes + n);
    now_ms = 100;
    guard_port.data = report; guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.routed == 1 && protected_link.handlers == 0 && protected_link.vehicle_handlers == 0);
    // Switching claimed peer/encoding cannot double the per-port rate budget.
    now_ms = 199;
    n = direct_observation(bytes, false);
    guard_port.data.assign(bytes, bytes + n); guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.routed == 1 && protected_link.droneid_filter.accepted[1] == 1);
    // Direct packet-dispatch calls cannot bypass the physical-port gate.
    mavlink_message_t command{};
    command.msgid = MAVLINK_MSG_ID_COMMAND_LONG;
    protected_link.GCS_MAVLINK::packetReceived(protected_link.status, command);
    assert(protected_link.handlers == 0 && protected_link.routed == 1);
    trusted_link.packetReceived(trusted_link.status, command);
    assert(trusted_link.handlers == 1); // Other physical ports retain behavior.
    // An unsigned report still fails the FC's configured signature requirement.
    mavlink_signing_t signing{};
    protected_link.status.signing = &signing;
    now_ms = 1000;
    guard_port.data = report; guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.routed == 1 && protected_link.status.signing == &signing);
    assert(protected_link.droneid_filter.accepted[1] == 2);
    // A spoofed-source command never reaches even the native MAVLink parser.
    mavlink_status_t tx{};
    mavlink_msg_command_long_pack_status(1, 243, &tx, &command, 1, 1, MAV_CMD_COMPONENT_ARM_DISARM,
        0, 1, 0, 0, 0, 0, 0, 0);
    n = mavlink_msg_to_send_buffer(bytes, &command);
    const unsigned parsed_before = protected_link.native_frames;
    guard_port.data.assign(bytes, bytes + n); guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.native_frames == parsed_before && protected_link.handlers == 0);
    // Standalone deployment: actual radio DRID with a nested SET_MODE frame
    // is normalized before the native parser; only the outer report is routed.
    now_ms = 2000;
    protected_link.status.signing = nullptr;
    n = direct_observation(bytes, false);
    std::vector<uint8_t> direct(bytes, bytes + n);
    guard_port.data = direct; guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.routed == 2 && protected_link.last_component == 242);
    assert(protected_link.handlers == 0 && protected_link.vehicle_handlers == 0);
    assert(protected_link.native_frames == parsed_before + 1);
    // Normalization cannot launder unsigned input through signing requirements.
    now_ms = 3000;
    protected_link.status.signing = &signing;
    guard_port.data = direct; guard_port.at = 0;
    drain(protected_link);
    assert(protected_link.routed == 2 && protected_link.native_frames == parsed_before + 1);
    // iNav has no forwarding path: count valid input; never execute it.
    mavlinkPort = &guard_port;
    guard_port.data = report; guard_port.at = 0;
    receiveDroneId();
    assert(droneidFilter.accepted[1] == 1);
    now_ms = 4000;
    guard_port.data = direct; guard_port.at = 0;
    receiveDroneId();
    assert(droneidFilter.accepted[1] == 2);
    guard_port.data.assign(10000, 0xfd); guard_port.at = 0;
    receiveDroneId();
    assert(guard_port.at == DRONEID_FC_READ_LIMIT);
    puts("FC adapters: physical-port isolation, report routing only, native signing retained, no local command dispatch, bounded reads passed");
}
