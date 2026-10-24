# Validation, evidence and open questions

Review date: **2026-10-24**. This is a source-based threat assessment with targeted
offline probes, not a penetration test of the installed aircraft.

## Reviewed baseline

Base commit: `48e9e01ea84999f7e8bc474ab4a766649235ac1c` (`init`). The working
tree also contained changes, including the USB Identify feature. Those files
were read as present; the review does not claim a clean committed release.
[baseline.json](evidence/baseline.json) records the changed paths and hashes of
the reviewed code, local build and relevant configuration. A local binary hash
does not establish that a connected board runs it or that it matches every
current source edit.

| Item | Reviewed value |
| --- | --- |
| Application / target | 0.5.0 / ESP32-C5, XIAO board |
| Local ESP-IDF checkout | v6.1 |
| OpenDroneID codec | `849c03adbe0cdd68997a462fafab397edcfe53f3`; see [upstream notes](../components/opendroneid/UPSTREAM.md) |
| MAVLink common C headers | `6a7760569ce6d46c56f93163703543d4345e940a`; see [upstream notes](../components/mavlink/UPSTREAM.md) |
| Host probe tools | Python 3.13.13, GCC 13.3.0; optional reference decoder pymavlink 2.4.50 |
| FC reference sources | ArduCopter 4.6.3 and iNav 9.0.0; not verified installed versions |
| Hardware deployment state | Not measured: eFuses, running images, FC parameters, downlink authentication and electrical isolation |

Security build observations: secure boot, flash encryption and NVS encryption
are not enabled. Compiler stack checking is set to NONE; task stack canaries
and ESP system memory protection are enabled. These are different controls.
Task watchdog support is enabled, but that alone does not prove timely recovery
from every overload. ROM secure-download configuration is not proof of per-device
eFuse enforcement. Keep these distinctions when comparing future builds.

## Existing automated tests

The existing host test script completed successfully during this review:
**two C test binaries under AddressSanitizer/UndefinedBehaviorSanitizer and
19 Python tests**, including decoding generated frames with reference pymavlink.
See [captured output](evidence/host-tests.txt).

For a host Python environment with the project's dependencies installed:

```sh
python3 -m pip install -r tools/requirements.txt -r tools/requirements-mavlink.txt
bash tests/run_host_tests.sh
```

The actual review used the already available optional pymavlink installation:

```sh
PYTHONPATH=/tmp/droneid-mavlink-python bash tests/run_host_tests.sh
```

That `/tmp` path is environment-specific, not a repository requirement. Without
pymavlink the reference comparison may be skipped; check the test output.

Coverage includes framing/truncation, JSON output bounds, upstream fixtures,
invalid configuration, CRC recovery, signed-input rejection, ages, queue/contact
bounds, browser origin guards and flash-layout validation. Randomized trials
include 10,000 BLE/codec inputs, 10,000 Wi-Fi iterations with both random and
header-seeded variants, and one million random UART bytes. These are finite
host checks, not coverage-guided exhaustive fuzzing or radio-hardware testing.

## Offline boundary probes

Run from the repository root with a C compiler and Python 3:

```sh
bash threatmodel/checks/run.sh
```

The script compiles into a temporary directory and removes it afterwards.
The C probes run with ASan/UBSan. Host probes import project modules and mock
the serial backend. They do not discover/connect devices, emit radio traffic,
send FC commands, contact a running HTTP server, or change saved device settings.
Temporary host logs are removed. Output is JSONL; the review output is saved in
[boundary-checks.jsonl](evidence/boundary-checks.jsonl).

| Probe | Observed result | Interpretation / limit |
| --- | --- | --- |
| Complete benign MAVLink heartbeat inside the 25 raw observation bytes | Exactly one outer TUNNEL frame, configured envelope IDs, raw bytes unchanged | Supports the normal serializer's data/command separation. Does not prove behavior after ESP32 compromise, serial corruption or arbitrary parser defects. |
| Same unsigned FC heartbeat replayed after expiry | Accepted without a key; replay refreshes heartbeat age | Confirms no authentication/replay gate for that telemetry path. No real controller involved. |
| One original contact followed by 128 distinct source MACs | Store remains at 128; original contact is evicted | Confirms bounded memory but vulnerable contact retention. The records are constructed offline; RF loss rates are not measured. |
| USB status containing numeric overflow `1e309` | Decoder accepts; `ValueError` escapes the real reader with mocked serial; snapshot fails JSON serialization | Confirms TM-07 host availability defect. It requires hostile USB output; an ordinary RF route to this status field was not found. |

**These are baseline evidence probes, not security acceptance tests.** Several
assert a known weakness. Exit code zero means the documented observations were
reproduced, not that the device is secure. When fixing a finding, replace the
corresponding probe with a rejection/recovery regression and preserve these
dated results as historical evidence.

## Source trace

| Boundary / property | Relevant code |
| --- | --- |
| Passive RF capture and nonconnectable transmission | [Wi-Fi](../main/wifi_radio.c), [BLE](../main/ble_radio.c), [scheduler](../main/radio.c) |
| Length validation, decoding, escaping, unverified identity | [Wi-Fi framing](../components/rid/rid_wifi.c), [RID serialization](../components/rid/rid.c), [vendored codec](../components/opendroneid/opendroneid.c) |
| Fixed TUNNEL envelope / unsigned FC telemetry | [fc_link](../components/fc_link/fc_link.c) |
| Queue limits, expiry, rate cap and independent worker | [fc_uart](../main/fc_uart.c) |
| USB commands, persistence, status | [application](../main/app_main.c) |
| USB JSON bounds and numeric-overflow acceptance | [LineDecoder](../tools/bench.py) |
| Reader failure, contact eviction, firmware bundle checks | [dashboard](../tools/dashboard.py) |
| Loopback/Host/Origin/token/CSP and resource assumptions | [server](../tools/server.py) |
| Browser rendering of untrusted text | [web app](../web/app.js) |
| Downlink report decoding and absent source/replay authentication | [base-station helper](../tools/mavlink_receiver.py) |

## Remaining validation

| Work | Method | Evidence needed |
| --- | --- | --- |
| Radio-stack robustness and starvation | Controlled RF test environment, no operational aircraft; malformed and valid-load traffic on each supported receive format | Peak CPU/heap, reset cause, loss/latency, queue/drop behavior, scan progress and recovery after traffic stops |
| FC isolation | Matching firmware in simulation, then isolated bench; test chosen independent guard or one-way wiring | Exact versions/configuration; rejected message classes; no unauthorized state changes; bounded processing and control-link latency |
| Real downlink trust | Map every participant/bridge and inspect signing/encryption/source policies | Injection/replay rejection, route behavior, session/reboot handling, reporter attribution |
| Build and physical access controls | Spare-board provisioning/recovery exercises and read-only eFuse inventory | Signed boot enforcement, debug/download state, approved recovery, unique key inventory |
| Host availability fix | Schema/error recovery regression with multiple mocked devices | Bad device cannot break shared state or healthy readers |
| UI content handling | Browser tests using hostile strings and malformed device data | Text-only display, bounded rendering, no script execution or unsafe export interpretation |
| Electrical containment | Non-flying bench with actual board revision and peripheral load | Supply margin; behavior under module reset, disconnect, short/current limiting and startup UART output |
| Dependency exposure | Pin SDK commit and radio-library versions; review relevant upstream advisories | Per-advisory applicability and remediation decision, not a generic ESP32 vulnerability list |

No hardware RF attack, live serial injection, port scan, eFuse operation, FC
configuration change or flash was part of this review. No radio remote-code-
execution vulnerability or successful FC takeover was demonstrated. The absence
of such a demonstration does not establish that those paths are impossible.

## Decisions needed for the next implementation milestone

1. Exact FC firmware/target/version and installed port/downlink configuration.
2. Whether to use physical one-way telemetry, an independent guard, or an FC-side
   policy; how reports reach the operator if ESP TX is disconnected.
3. Whether the downlink already enforces signing/authentication, and which
   participants currently possess command-authorizing keys.
4. Whether captured modules, untrusted maintenance users and remote bench access
   are in deployment scope; required recovery and log-retention policy.
5. Required observation capacity and acceptable loss/latency under overload.

These do not prevent documenting the current stance. They determine the
deployment policy and the hardware tests needed to claim containment.

## External references

Retrieved during this review on 2026-10-24. Controller links are pinned reference
versions; the installed vehicle may differ. The vendor documentation/advisory
index is live and must be rechecked for a release.

- [iNav 9.0.0 MAVLink implementation](https://github.com/iNavFlight/inav/blob/9.0.0/src/main/telemetry/mavlink.c): incoming-message dispatch and handlers discussed in TM-04.
- [ArduCopter 4.6.3 GCS implementation](https://github.com/ArduPilot/ardupilot/blob/Copter-4.6.3/libraries/GCS_MAVLink/GCS_Common.cpp): packet acceptance example; [ArduPilot routing](https://ardupilot.org/dev/docs/mavlink-routing-in-ardupilot.html): target/routing behavior.
- [MAVLink message signing](https://mavlink.io/en/guide/message_signing.html): authentication/replay mechanism; [TUNNEL protocol](https://mavlink.io/en/services/tunnel.html): opaque payload transport.
- [ESP32-C5 security overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/security/security.html): platform security mechanisms; [ESP-IDF advisories](https://github.com/espressif/esp-idf/security/advisories): dependency triage source.
- [RFC 9434, DRIP architecture](https://www.rfc-editor.org/rfc/rfc9434.html): Remote ID trust and authentication context.
