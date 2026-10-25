# USB bench mode

The same firmware supports a USB-connected receiver and independently powered
test transmitters. USB carries **schema-1 newline-delimited JSON (JSONL)** as the
bench substitute for the future flight-controller connection. It is not binary
MAVLink. The Remote ID observations are independent of the USB transport so a
MAVLink 2 UART reporting adapter now runs alongside USB; see the
[UART contract](uart-protocol.md).

## Supported scope

- Receive BLE legacy Remote ID service data (UUID `0xFFFA`, application `0x0D`).
- Receive Wi-Fi Beacon vendor IEs (`FA:0B:BC`, type `0x0D`) and Wi-Fi NAN Remote
  ID service descriptors. Split each validated pack into individual messages.
- Decode single Basic ID, Location, Self ID, System, Operator ID, and Authentication
  messages using the pinned OpenDroneID codec. Authentication is not verified.
- Preserve each packet's RSSI, address/type, receive timestamp, message counter,
  protocol version, and exact 25-byte message.
- Emit periodic module status and explicit queue/USB loss counters.
- Transmit experimental Basic ID and Self ID using **one** saved transport:
  Wi-Fi Beacon (default) or BLE legacy.

BLE extended/coded-PHY advertisements and Wi-Fi NAN transmission are not implemented.
Firmware 0.5.0 adds a separately configured MAVLink 2 hardware UART; USB remains JSONL. There is no
synthetic aircraft position in the test broadcasts. This transmitter is a test
source, not a complete US Remote ID broadcast module.

## Listening on both radios

One C5 shares its radio path between Wi-Fi (including 5 GHz) and BLE. The receiver
uses explicit windows rather than leaving both capture procedures running:

| Window | Nominal duration | Captured packets |
| --- | --- | --- |
| Wi-Fi channel 6, 2437 MHz | 300 ms | Beacon and NAN |
| Wi-Fi channel 149, 5745 MHz | 300 ms | Beacon and NAN |
| BLE advertising channels | 400–500 ms | Legacy Remote ID service data |

The order of the two Wi-Fi windows is randomized each cycle, and the BLE window
varies, to reduce persistent alignment with a peer's periodic broadcasts.
Switching/driver overhead adds to these times. Wi-Fi capture is
disabled before BLE scanning starts, and BLE scanning/advertising stops before
returning to Wi-Fi. Wi-Fi never joins a network; BLE uses passive scanning.

Packets sent outside a matching receive window can be missed. This is not
continuous simultaneous coverage; other Wi-Fi channels and BLE coded/extended
advertising remain outside this version's coverage. Status reports the current
window and accumulated dwell times, which are not a measurement of RF delivery
or packet-loss rate. See [Espressif's shared-radio limitations](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/api-guides/coexist.html).

In `both` mode the selected transmitter shares these windows: Wi-Fi packs are
sent three times during the channel-6 window, or BLE Basic ID/Self ID alternate
during the BLE window. In `tx` mode there is no receive schedule: Wi-Fi sends
about every 100 ms; BLE updates the advertised message every 500 ms. These are
bench timings, not a validated regulatory broadcast schedule.

## Configure multiple boards

The [web bench](web-bench.md) provides the same configuration workflow plus
flashing and a graphical live view. The commands below are useful without the
web server, or after pausing the selected board's web monitor.

Install `tools/requirements.txt` into your chosen Python environment; the
ESP-IDF Python environment already includes pyserial in the current setup.

```sh
python -m pip install -r tools/requirements.txt
python tools/bench.py list
```

Build and flash the same image to each XIAO:

```sh
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM1 flash
```

These port names are examples: use the ports reported by `list`. Prefer
`/dev/serial/by-id/...` paths on Linux when reconnecting boards. Close VS Code's
serial monitor before using the host tool; each port should have one reader.

Boards initially default to `rx`. Configure the roles explicitly:

```sh
python tools/bench.py role rx --port /dev/ttyACM0
python tools/bench.py role tx --port /dev/ttyACM1
python tools/bench.py transport wifi --port /dev/ttyACM1
python tools/bench.py status --port /dev/ttyACM0
python tools/bench.py monitor --port /dev/ttyACM0 --output received.jsonl
```

A role change is saved in NVS and restarts the board. A transmitter retains its
role after losing power, so it can then run from a USB charger or power bank.
`role both` enables the selected test transmitter alongside reception on both
protocols. It does not mean broadcasting on two transports. Set
`role rx` to stop transmitting. Each board derives a unique experimental session
ID from its factory MAC by default; the ID begins with private experimental type
`0xFF` and the label `BENCH-...`. The default Self ID says `BENCH TEST - NO FLIGHT`.
Firmware 0.4.0 lets you change both labels and all radio settings in one atomic
NVS commit:

```sh
python tools/bench.py configure --port /dev/ttyACM0 --role both --transport wifi --id SCOUT-01 --hello "Hello from scout 01"
```

The ID accepts 1–19 ASCII letters, numbers, dots, underscores, or hyphens. The
hello message accepts 0–23 printable ASCII characters; use `--hello ""` to clear
it. These remain experimental Session IDs and public Self ID descriptions.
Each board can receive its peers while broadcasting its own settings in `both`
mode. Changes restart only that board. Existing role/transport commands preserve
the configured ID and hello message.

Use `transport wifi` (default, Beacon on channel 6) or `transport ble` to choose
the sole broadcast transport. Both choices persist and restart the board.
`transport both` is rejected. The receiver still listens to BLE and Wi-Fi when
the role is `rx` or `both`. Upgrading from 0.2.0 preserves roles and selects Wi-Fi
if no transport setting has been saved. A receiver-only board remains silent.

Flash the complete current build, including its custom partition table: adding
Wi-Fi needs more than the old 1 MB app partition. `partitions.csv` expands the
app to 2 MB without moving NVS or PHY data.

To print machine-readable records or stop after a fixed interval:

```sh
python tools/bench.py monitor --port /dev/ttyACM0 --json --duration 30
```

`--output` appends JSONL including host UTC reception times. The monitor retries
the same port after disconnects; a stable device path avoids following the wrong
board. The firmware and host parser tolerate a partial line at reconnect.

On the Linux host used for verification, opening a serial connection restarted
the board. Keep `monitor` running for a continuous capture; opening another
command can begin a new boot session. Saved roles persist, and `session` lets
the base station distinguish restarted uptime and sequence numbers.

## Host validation

The CLI and web bench validate device records with
[`tools/usb_records.py`](../tools/usb_records.py) before using them. Accepted wire
input is UTF-8 schema-1 JSONL, at most 4,096 bytes per line. The host rejects
duplicate keys, unsupported kinds/fields, excessive nesting (maximum depth 6),
oversized structures (256 visited values/keys), non-finite numbers, out-of-range
integers and wrong field types, including booleans in numeric fields. Oversized
or malformed lines are discarded through their newline; the next valid record
can be accepted on the same stream.

Schemas cover Scout status, guard status, replies, observation metadata and all
currently decoded RID data objects, including nested UART/position/GPS and guard
counters. Identity/session and kind-specific core fields are required. Optional
telemetry/identify fields permit older known schema-1 records; whenever present,
they are validated too. Unknown fields fail closed, so a firmware extension must
update the host schema. Firmware predating the configured-ID status fields needs
an update before using this host. Valid observations remain unauthenticated claims.

The dashboard revalidates and copies input, and prepares serialization before
changing status, sessions, contacts or logs. Invalid input cannot clear contacts
by supplying a new session. Unexpected reader/driver exceptions are isolated to
that device and retried after a delay. The selected-device view and `/api/state`
expose `usb_input.rejected_lines`, `reader_failures`, `last_error` and its UTC time;
counts persist for that device's lifetime in the server. Rejected lines can
include boot text or partial reconnect data. A recovered status clears the active
error, while counters retain the history. These checks do not establish device
authenticity or a global throughput/flood budget.

## Observation contract

Each `kind: "observation"` line describes one received message, not an aggregated
aircraft record. Other kinds are `status` and `reply`; all carry `schema: 1`.

| Field | Meaning |
| --- | --- |
| `receiver_id` | Receiver factory MAC in lowercase hex |
| `session` | Random boot-session tag; separates resets and sequence restarts |
| `seq` | Receiver-local observation sequence, incremented before queueing |
| `rx_us` | Monotonic receive time, microseconds since this receiver booted |
| `reported_us` | Receiver uptime when formatting the record; exposes queue age |
| `rssi_dbm` | Measured signal strength for this packet, or null if unavailable |
| `source_address`, `source_address_type` | BLE source and NimBLE address-type code, or Wi-Fi source MAC and null type |
| `transport` | `ble_legacy`, `wifi_beacon`, or `wifi_nan` |
| `phy` | `1M` for legacy BLE; null for Wi-Fi (modulation is not reported) |
| `channel`, `frequency_mhz` | Actual Wi-Fi receive channel/frequency; null for BLE because legacy HCI omits the channel |
| `pack_index`, `pack_size` | Zero-based index/count within a Wi-Fi message pack; 0/1 for legacy BLE |
| `message_counter` | Sender's eight-bit counter, per message type for BLE or per Wi-Fi pack; not a reliable RF packet-loss counter |
| `message_type`, `protocol_version` | Remote ID message header |
| `raw_message_hex` | All 25 payload bytes, including unknown/reserved fields |
| `decode_status` | `ok`, `invalid`, or `unsupported` |
| `authentication` | `unverified` |
| `data` | Fields decoded from this message only, or null |
| `host_received_utc` | Added by the Python logger; not an aircraft timestamp |

Less negative RSSI normally means a stronger received signal. RSSI is not a
distance measurement. Repeated advertisements are intentionally retained to
observe signal variation. Basic IDs and positions arrive in separate messages;
the base station should preserve their individual ages. An observed address or
ID is not proof of a unique physical aircraft.

Unknown location/altitude/speed/time values are represented as null where the
wire format defines a sentinel. Raw bytes always remain available. Binary IDs
are preserved in `data.uas_id_hex`; `uas_id_text` is only populated for text ID
types. The location timestamp is seconds within an hour, not Unix time.

## Offline behavior and diagnostics

The receiver has a bounded 64-record queue. Radio callbacks copy packet bytes and
never wait for USB. USB writes have a bounded wait; absent/slow readers cause
counted drops. This version does not retain a complete offline log or replay
missed observations after reconnection. A few buffered records may arrive on
reconnect with their original receive times.

Status records occur every two seconds. `rx_queue_dropped` counts observations
lost before formatting. `usb_records_dropped` counts records not accepted by
the USB output path, including status records. Driver-accepted bytes are not an
end-to-end delivery acknowledgement. `tx_advertisement_sets` counts accepted
advertising configurations, not verified packets received over the air.

`wifi_tx_frames` counts accepted Wi-Fi transmit calls, not delivery. The inactive
transmitter's counter stays zero. `ble_observations` and `wifi_observations`
count decoded framing records; a two-message Wi-Fi pack contributes two.
`wifi_frames_seen` counts management frames examined, including unrelated ones.
`malformed_advertisements` includes malformed BLE advertisements or Wi-Fi frames.

`rx_transports` lists enabled receive adapters (empty in `tx`); `tx_transport`
is the saved `wifi_beacon` or `ble_legacy` choice, even in receiver-only mode.
`rx_phase` is `idle`, `wifi_6`, `wifi_149`, or `ble`. `scan_cycles` counts completed
schedules. `ble_listen_ms`, `wifi_6_listen_ms`, and `wifi_149_listen_ms` accumulate
completed windows, excluding the unfinished current window; these 32-bit
counters can wrap. Receive times and sequence values remain per boot session.

Typed commands are `status [token]`, `role rx|tx|both [token]`, and
`transport wifi|ble [token]`, each terminated
by a newline. Replies echo the optional alphanumeric token. Role writes are
acknowledged after NVS commit and followed by a restart. The Python tool generates
tokens so it does not confuse queued status records with command replies.

The configuration command is `configure <rx|tx|both> <wifi|ble> <id_hex>
<hello_hex|-> <token>`, on one line. The CLI handles encoding and validation;
`-` represents an empty hello message. Firmware validates the whole request
before saving one versioned settings blob. Status includes `configuration_version`,
`broadcast_id`, and `hello_hex`, which the web app decodes for display.

## Verification

Run host tests without connecting hardware:

```sh
bash tests/run_host_tests.sh
```

The tests cover fixed packet fixtures, truncation, wrong service codes, JSON
escaping, binary IDs, unknown values, unsupported protocol versions, bounded
output, and host stream recovery. Wi-Fi fixtures were generated by the pinned
upstream OpenDroneID builders; tests cover all truncation points and nested
length mutations. C tests run with AddressSanitizer and UndefinedBehaviorSanitizer,
including 10,000 BLE and 10,000 Wi-Fi arbitrary-payload trials.

For radio validation, confirm that a second board's Basic ID and Self ID appear
with non-null RSSI, then move the transmitter and compare measurements. Test
USB reconnection and `both` mode separately. An independent Remote ID receiver
is still needed to establish interoperability beyond this pair of boards.

### BLE/Wi-Fi hardware results — 2026-10-23

Firmware `0.3.0` builds with ESP-IDF v6.1 and passes the host sanitizer/stream
tests. Both physical boards were upgraded, including the larger app partition.

- In one uninterrupted receiver session, a 10-second Wi-Fi stage captured 54
  message records (27 Basic ID / Self ID pairs) at −33 to −32 dBm on channel 6.
  After switching only the transmitting board to BLE, the next 10-second stage
  captured 38 BLE messages at −46 to −42 dBm. All sampled radio-error and
  queue/USB-drop counters were zero. All three listen-time counters advanced.
- In a simultaneous 15-second `both`-role test, the Wi-Fi-transmitting original
  board received 48 BLE messages at −47 to −43 dBm. The BLE-transmitting second
  board received 52 Wi-Fi records at −33 to −31 dBm. Each board remained in one
  boot session with zero reported radio errors or queue/USB drops. The inactive
  transmit counter stayed zero on each board.
- Both boards rejected `transport both`. The selected transport persisted
  through subsequent serial-open restarts. The upgrade selected Wi-Fi without
  overwriting the original saved `rx`/`tx` roles.
- A temporary transmitter built from the unmodified pinned upstream OpenDroneID
  Wi-Fi builders alternated Beacon and NAN test packs on channels 6 and 149.
  A 26-second capture decoded all four combinations: 36 Beacon records and 36
  NAN records on channel 6, plus 38 Beacon records and 36 NAN records on channel
  149. All records had valid decodes and RSSI; all sampled error/drop counters
  were zero. This tests an independent frame builder, not a commercial receiver
  or regulatory certification. The temporary firmware was then removed.

Local captures: `logs/receive-wifi-03.jsonl`, `logs/receive-ble-03.jsonl`,
`logs/mixed-both-original-03.jsonl`, `logs/mixed-both-second-03.jsonl`, and
`logs/wifi-transmitter-03.jsonl`. Reference capture:
`logs/upstream-wifi-reference-03.jsonl`. Record counts are not RF packet counts: Wi-Fi
packs contain two messages, BLE advertisements one. These tests establish bench
function, not measured range, continuous coverage, or a loss-rate guarantee.

Saved setup after the 0.3.0 tests: original board `10:BD:A3:CA:38:60` was `rx`, listening on both
protocols; second board `10:BD:A3:CA:21:40` is `tx` with `transport wifi`.
Both run the project firmware, and the restored setup was checked in
`logs/final-wifi-default-03.jsonl`. Use `role both` on a selected board to enable
reception alongside its one configured transmitter.

### Earlier BLE-only hardware results — 2026-10-23

Firmware `0.2.0` was built with ESP-IDF v6.1 and flashed successfully to both
XIAO ESP32-C5 boards (chip revision 1.0). The host tests passed.

| Board factory MAC | Saved bench role | BLE source address |
| --- | --- | --- |
| `10:BD:A3:CA:38:60` | `rx` | `10:bd:a3:ca:38:62` when transmitting |
| `10:BD:A3:CA:21:40` | `tx` | `10:bd:a3:ca:21:42` |

- A 12-second receiver capture collected 104 messages from the second board:
  51 Basic ID and 53 Self ID, with RSSI from −45 to −41 dBm.
- With both boards in `both` mode, a simultaneous 10-second capture collected
  65 messages on each board (35 Basic ID and 30 Self ID). RSSI ranged from −46
  to −42 dBm on the original board and −47 to −41 dBm on the second board.
  Every sampled status reported zero radio errors, queue drops, and USB drops.
- A transmitter-only capture showed the advertising configuration count rising
  from 1 to 13 over about six seconds, with zero reported radio errors.
- After restoring `rx`/`tx`, an eight-second receiver capture collected 21
  messages (11 Basic ID and 10 Self ID), with RSSI from −46 to −42 dBm and
  zero reported radio errors or queue/USB drops.
- Roles persisted across software/serial-open resets. Physical power-bank
  operation, measured range, and third-party interoperability remain untested.

Captures are saved locally under the ignored `logs/` directory, including
`receiver-first.jsonl`, `combined-original.jsonl`, `combined-second.jsonl`, and
`transmitter-status-final.jsonl`. The restored-role capture is
`receiver-final.jsonl`. These are short bench checks, not reception-rate or range
guarantees. The two boards were restored to the roles in the table.
