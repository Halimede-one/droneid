# Flight-controller and base-station integration

Status: ESP32-side UART/MAVLink 2 adapter implemented in firmware 0.5.0,
2026-10-23. See the [README wiring instructions](../README.md#flight-controller-wiring-and-uart-setup)
and [UART wire contract](uart-protocol.md). The first board is **MicoAir H743 V2
AIO**, with a UART4 example and annotated wiring image in the README. The
installed firmware version, available ports, electrical connection, and
end-to-end forwarding still need bench verification. USB JSONL remains available
alongside UART.

The intended vehicle path reports nearby aircraft to the operator through the
existing MAVLink telemetry stream. The base station runs custom parsing software.
iNav is the preferred flight-controller firmware; ArduPilot and Betaflight
remain candidates.

## Two data flows and current scope

1. **Own aircraft:** flight controller → UART → ESP32 → Remote ID broadcast.
   Aircraft identity and any unavailable operator/takeoff data need explicit
   configuration or another valid source.
2. **Received observations:** nearby broadcast → ESP32 → UART → flight-controller
   forwarding → existing MAVLink downlink → custom base-station parser and log.

The current implementation forwards received observations using standard TUNNEL
messages with a versioned payload. Incoming controller heartbeat, global position,
and GPS quality are monitored in USB status. Using that data for own-aircraft
Remote ID broadcasts is still future work.

Own-aircraft transmit data and received-aircraft observations have distinct
ownership. A received observation must never replace the own-aircraft identity
or position used by the transmitter.

## Firmware compatibility to establish

| Firmware | Proposed own-aircraft input | Return path to custom base station |
| --- | --- | --- |
| iNav, primary candidate | Selected version's MAVLink telemetry; MSP is not implemented in this module | Current stock telemetry code has no general TUNNEL forwarding path; requires an extension or explicit gateway |
| ArduPilot, alternative | MAVLink 2, including its Remote ID integration where available | Documented MAVLink routing makes this the most direct candidate; validate port settings, routing, and actual message delivery |
| Betaflight, optional later | Assess MSP fields in the chosen build | Unverified; requires a separate integration investigation before promising support |

iNav documents [serial-port functions](https://github.com/iNavFlight/inav/blob/master/docs/Serial.md)
and its [MAVLink interface](https://github.com/iNavFlight/inav/wiki/Mavlink).
ArduPilot documents [Remote ID integration](https://ardupilot.org/copter/docs/common-remoteid.html)
and [routing across MAVLink ports](https://ardupilot.org/dev/docs/mavlink-routing-in-ardupilot.html).
The preference for ArduPilot's return path is an engineering inference from that
documented routing support, not a completed compatibility test.

To carry these reports through iNav, the decision is an
iNav extension, ArduPilot, or an explicit telemetry gateway. A gateway can combine
data into the existing downlink, but would change the UART topology. That choice
requires the installed firmware and telemetry-link details. The README includes
[ArduPilot SERIAL4 setup and iNav UART4 limitations](../README.md#controller-and-base-station-connection).
The example assumes UART4 is available; the supplied MicoAir wiring diagram
otherwise assigns it to optical flow.

## MAVLink contract

The implemented reporting path uses **MAVLink 2**. The
[Open Drone ID service](https://mavlink.io/en/services/opendroneid.html) already
defines messages for passing identity data to/from a Remote ID component and
reporting received aircraft. This prototype uses TUNNEL to retain raw messages plus RSSI and local receive
timestamps together; full Open Drone ID service integration is future work. Verify
that every forwarding hop preserves them; a MAVLink 1-only hop is insufficient
for their message IDs.

Module health, receive timestamps, signal measurements, and queue-loss metadata
are explicit in the [versioned TUNNEL payload](uart-protocol.md). No custom
MAVLink dialect is required, but the selected controller build must recognize
and forward the standard TUNNEL envelope.

The module needs a configured MAVLink system/component identity that does not
collide with existing equipment. Observed aircraft IDs belong in message
payloads; they must not be mistaken for the transmitting module's identity.

Do not assume that the flight controller's native log captures forwarded
observations. Persistent logging is the responsibility of the custom base
station in the proposed first version.

## Proposed observation content

| Content | Meaning |
| --- | --- |
| Observer session and sequence | Identify the receiving module's boot session and detect reporting gaps |
| Claimed IDs and types | Preserve the broadcast's identifier values and identifier types |
| Radio source | Transport, source address when available, channel/PHY when available, and RSSI |
| Time and freshness | Monotonic receive time plus field ages; claimed sender time stored separately |
| Aircraft data | Position, altitude with reference, velocity, and validity/accuracy information where available |
| Decode/authentication status | Distinguish valid syntax from authenticated identity; default to unverified |
| Module health | Input age, queue losses, radio errors, and reception availability |

The base station adds its own reception time. RSSI is a signal measurement, not
a calibrated range estimate. Missing fields remain unknown. Repeated messages
may be coalesced under bandwidth limits, with the number of omitted updates
reported. Observations expire visibly rather than remaining current forever.

## UART and link budget

The ESP32 uses hardware UART1 with configurable pins and baud rate. Confirm
voltage levels, common ground, and TX/RX routing from the actual board diagrams.
Keep diagnostic console bytes off the flight-controller protocol port. The
current firmware uses USB Serial/JTAG with ordinary logs disabled. The
flight-controller UART defaults to disabled until configured over USB. Defaults
are D6/GPIO11 TX, D7/GPIO12 RX, 115200 8N1. The README contains the wiring table.

The flight controller's ports are independent protocol endpoints; they are not
automatically a transparent serial bridge. Verify both own-aircraft input and
observation output, especially if they need different protocols on iNav.

Budget for serialized message bytes, packet rate, signing overhead if used,
and the telemetry link's remaining capacity. Rate-limit observation reporting
so it fits alongside existing traffic. Maintain bounded queues and report
congestion. A disconnected or slow downlink must not block local radio work.

## First integration test

On the bench, send a synthetic observation through the complete intended path
and confirm its fields in the custom base-station parser. Test while ordinary
telemetry is also flowing. Then verify disconnect/reconnect, congestion, and
module restart behavior. This proves the transport before radio decoding and
own-aircraft transmission are added to it.
