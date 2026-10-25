// Included by telemetry/mavlink.c in the pinned iNav integration.
static droneid_fc_filter_t droneidFilter;

static void receiveDroneId(void)
{
    const uint32_t now = millis();
    for (unsigned i = 0; i < DRONEID_FC_READ_LIMIT && serialRxBytesWaiting(mavlinkPort); ++i) {
        (void)droneid_fc_feed(&droneidFilter, serialRead(mavlinkPort), now);
    }
    // iNav 9.0.0 has no TUNNEL router. Accepted frames are counted and consumed,
    // never handed to mission/RC/parameter/ADSB handlers. Do not unwrap DRID
    // into an existing MAVLink command or sensor-injection handler here.
}
