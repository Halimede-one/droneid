// Included by GCS_Common.cpp in the pinned ArduCopter integration.
bool GCS_MAVLINK::droneid_uart() const
{
    // SERIAL index is physical-port policy; MAVLink channel indices change
    // when other telemetry ports are enabled. Packet source IDs are spoofable.
    return _port != nullptr &&
        _port == AP::serialmanager().get_serial_by_id(DRONEID_FC_SERIAL);
}

bool GCS_MAVLINK::route_droneid_packet(const mavlink_message_t &msg)
{
    if (!droneid_uart()) return false;
    // Never enter local command, failsafe-heartbeat, sensor or scripting
    // handlers. Heartbeats are consumed; only validated reports are routed.
    if (droneid_dispatch && msg.msgid == MAVLINK_MSG_ID_TUNNEL) {
        (void)routing.check_and_forward(*this, msg);
    }
    return true;
}

void GCS_MAVLINK::update_droneid_receive()
{
    const uint32_t now = AP_HAL::millis();
    const uint32_t available = _port->available();
    const uint32_t count = MIN(available, DRONEID_FC_READ_LIMIT);
    for (uint32_t i = 0; i < count; ++i) {
        const int16_t byte = _port->read();
        if (byte < 0) break;
        const unsigned length = droneid_fc_feed(&droneid_filter, (uint8_t)byte, now);
        if (!length) continue;
        // Input CRC/schema were checked before normalization. Feed only the
        // complete encoded frame, preserving native unsigned-frame policy.
        // Reset framing only; retain signing state and keys.
        channel_status()->parse_state = MAVLINK_PARSE_STATE_IDLE;
        mavlink_message_t msg;
        mavlink_status_t status;
        for (unsigned j = 0; j < length; ++j) {
            const uint8_t result = mavlink_frame_char_buffer(channel_buffer(), channel_status(),
                                                           droneid_filter.frame[j], &msg, &status);
            if (result == MAVLINK_FRAMING_OK) {
                droneid_dispatch = true;
                // Bypass vehicle overrides too: Copter's override calls
                // avoidance/follow consumers before its base implementation.
                GCS_MAVLINK::packetReceived(status, msg);
                droneid_dispatch = false;
            }
        }
        // No alternative protocol, bad-CRC scripting hook or ordinary command
        // dispatch is reachable from bytes on this physical port.
    }
}
