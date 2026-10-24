# UART / MAVLink 2 contract — firmware 0.5.0

Wiring and startup instructions are in the [README](../README.md#flight-controller-wiring-and-uart-setup).
This is the ESP32-side transport implementation. A physical controller/downlink
round trip is not yet verified. USB remains schema-1 JSONL; hardware UART1 carries
binary MAVLink. Both outputs receive independent copies of radio observations.
The default D6 TX pin can carry the chip's ROM startup banner before application
initialization. Receivers must resynchronize after noise/reset, as the standard
MAVLink parser does; no eFuse changes are made to suppress ROM output.

## MAVLink envelope

The module sends HEARTBEAT (message 0) at 1 Hz as `MAV_TYPE_ONBOARD_CONTROLLER`,
`MAV_AUTOPILOT_INVALID`, with no flight-control mode flags. Source IDs default to
vehicle system 1 and component 242 (tunnel node). Configure unique IDs.

Reports use the standard common-dialect TUNNEL message (385), requiring MAVLink 2.
`payload_type=0` (unknown/vendor-defined); `payload` starts with `DRID` and an
explicit version/kind. We do not allocate a private common-dialect message ID
or claim a registered TUNNEL payload type. Default destination is system 255,
component 190. Zero destination fields permit broadcast; directed routing is
preferred. Send base-station heartbeats so the controller learns its route.

The standard Open Drone ID messages do not contain this receiver's RSSI,
per-observation receive time and queue-loss metadata. One compact TUNNEL record
keeps those attached to the original raw message without duplicating traffic.
It requires the custom base-station decoder; ordinary GCS aircraft displays
will not automatically render these reports. See the
[TUNNEL definition](https://mavlink.io/en/messages/common.html#TUNNEL) and
[Open Drone ID service](https://mavlink.io/en/services/opendroneid.html).

## Payload v1

All multibyte integers are little endian. No C struct padding goes on the wire.
`payload_length` is 78 for observations, 84 for health. MAVLink 2 may trim trailing
zero bytes from its own frame; a standard decoder restores the zero-filled
array before interpreting `payload_length`.

| Offset | Size | Shared field |
| --- | --- | --- |
| 0 | 4 | ASCII `DRID` |
| 4 | 1 | Payload version = 1 |
| 5 | 1 | Kind: 1 observation, 2 health |
| 6 | 6 | Receiver factory MAC, display/network byte order |
| 12 | 8 | Boot session; same number as the hexadecimal USB session string |
| 20 | 4 | Observation sequence, or independent health-record sequence |
| 24 | 8 | Receive time in microseconds since receiver boot; health uses report time |
| 32 | 8 | Reporting time in microseconds since receiver boot |

Observation fields:

| Offset | Size | Field |
| --- | --- | --- |
| 40 | 1 | Transport: 0 BLE legacy, 1 Wi-Fi Beacon, 2 Wi-Fi NAN |
| 41 | 1 | Signed RSSI in dBm; -128 means unavailable |
| 42 | 6 | Transmitter address, display/network byte order |
| 48 | 1 | NimBLE address type for BLE; 255 for Wi-Fi |
| 49 | 1 | Receive channel; 0 unavailable (BLE legacy) |
| 50 | 1 | Sender's message/pack counter |
| 51 | 1 | Zero-based index within pack |
| 52 | 1 | Pack size; BLE uses 1 |
| 53 | 25 | Exact Remote ID message bytes, including header and reserved data |

Health fields are eleven uint32 values starting at offset 40:

| Offset | Field |
| --- | --- |
| 40 | UART observation queue drops |
| 44 | UART observations discarded after exceeding two seconds of age |
| 48 | Complete outbound MAVLink frames submitted to UART FIFO |
| 52 | Checksum-valid inbound common-dialect frames, regardless of source |
| 56 | Inbound CRC/signature framing failures |
| 60 | UART hardware/initialization/transmit errors |
| 64 | Controller heartbeat age in milliseconds; UINT32_MAX if absent |
| 68 | Radio errors |
| 72 | USB observation queue drops |
| 76 | USB records dropped |
| 80 | Total radio observations |

Counters reset on boot and can wrap. Session/sequence distinguish restarts and
missing reports. Health emits every two seconds. No message is authenticated
by this record format; base-station parsing must retain that distinction.

## Capacity and input behavior

The default cap is five observation **messages** per second, shared across all
sources. A two-message Wi-Fi pack consumes two slots. The FIFO holds 32 records;
full queues drop the new record, and records older than two seconds are discarded
before transmission. USB has a separate queue. Heartbeat and health take priority
over observations. Repeated messages are not coalesced in this version.

An observation requires at most 95 UART bytes without signing; health at most
101, heartbeat 21. The default maximum is about 547 bytes/second before other
traffic. MAVLink zero trimming may reduce this. At 50 observations/second, budget
about 4.9 kB/second. Budget the whole radio/downlink, not just the UART baud rate.
There is no RTS/CTS and no delivery acknowledgment or backpressure from the base
station. A disconnected receiver does not stop UART transmission. Drop counters
describe local capacity, not RF loss or downstream forwarding loss.

Incoming MAVLink 1 or 2 common frames are parsed by the pinned official C library.
Only the configured vehicle system and controller component update telemetry.
A non-GCS heartbeat with a valid autopilot type must arrive before position/GPS
updates are accepted. Each field group keeps its own local arrival time; five
seconds without a fresh heartbeat or field update makes that data unavailable in
USB status. The received position is diagnostic, not an independently verified
navigation fix. `GPS_RAW_INT.fix_type` and its age remain separate.

Outgoing frames are unsigned. Signed input is ignored because this build has no
signing-key provisioning or verification policy. It sends no arm, mode, mission,
parameter, or stream-rate commands, and accepts no remote configuration commands.
Configure controller telemetry output separately. Reconnect needs no handshake;
new valid heartbeats restore the telemetry indication. Incoming aircraft data
does not change configured RF identity or create own-aircraft Location broadcasts.

## Saved configuration and USB command

NVS blob `uart_v1` is independent of `settings_v1`. Missing or invalid UART settings
leave UART disabled. Configuration is committed atomically, followed by a restart.
The status field `uart` reports saved configuration plus runtime counters and ages.

One ASCII command line, normally generated by `tools/bench.py`:

```text
uart <enabled:0|1> <baud> <tx_gpio> <rx_gpio> <system_id> <component_id> <fc_component_id> <target_system> <target_component> <max_rate> <request_token>
```

Only supported baud rates and XIAO D0–D10 GPIOs are accepted. Pin collisions,
zero source IDs, component collisions with the FC, oversized values and invalid
rates are rejected before saving. Acknowledgments use the existing USB request
token convention. The web app verifies the saved settings after restart.

`tools/mavlink_receiver.py` is a reusable decoder example. It validates payload
length/version/metadata, prints raw observations and health, and extracts Basic ID
and Self ID labels for convenience. It does not implement the complete OpenDroneID
flight-data decoder; use the raw message with the pinned codec for those fields.
