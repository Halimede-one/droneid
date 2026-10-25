# Validation, evidence and open questions

Review date: **2026-10-24**. This is a source-based threat assessment with targeted
offline probes, not a penetration test of the installed aircraft.

For the subsequent independent-guard implementation, see the
[P0 build/test evidence](evidence/p0-implementation.json) and
[guard acceptance guide](../guard/README.md#validation-and-remaining-acceptance).
The standalone FC alternative has its own [build/test evidence](evidence/p0-fc-standalone.json)
and [integration/acceptance guide](../fc_filter/README.md#verification-and-acceptance).
The earlier [FC prototype snapshot](evidence/p0-fc-filter.json) records the
superseded guard-only input profile. The current standalone implementation also
accepts direct radio DRID, so the two P0 implementations are alternatives.
The baseline results below remain historical evidence; no physical P0 tests
have been completed.

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
| USB status containing numeric overflow `1e309` | Originally accepted and broke the reader; now rejected, then a valid status is accepted with serializable state | Real-reader regression with mocked serial. P1 adds full current-record schemas and multi-device isolation; historical baseline evidence is preserved. |

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
| USB JSON bounds, duplicate-key rejection, numeric/field schemas | [LineDecoder](../tools/bench.py), [USB schemas](../tools/usb_records.py) |
| Reader failure, contact eviction, firmware bundle checks | [dashboard](../tools/dashboard.py) |
| Loopback/Host/Origin/token/CSP and resource assumptions | [server](../tools/server.py) |
| Browser rendering of untrusted text | [web app](../web/app.js) |
| Downlink report decoding and absent source/replay authentication | [base-station helper](../tools/mavlink_receiver.py) |

## Remaining validation

The first P1 USB-input item is complete: see
[P1 USB validation and reader isolation](#p1-usb-validation-and-reader-isolation).

The deployment-specific [P0 closure checklist](p0-acceptance.md) is the current
acceptance record. P0 is **partially closed, pending full validation**, at the
user's direction. The [P0 verification plan](p0-verification-plan.md) preserves
the deferred procedures and evidence requirements for a later session.
The designated C5 guard has passed USB installation/startup
verification, but every FC/harness integration gate remains open; iNav observation
forwarding is also an unimplemented functional path.

| Work | Method | Evidence needed |
| --- | --- | --- |
| Radio-stack robustness and starvation | Controlled RF test environment, no operational aircraft; malformed and valid-load traffic on each supported receive format | Peak CPU/heap, reset cause, loss/latency, queue/drop behavior, scan progress and recovery after traffic stops |
| FC isolation | Matching firmware in simulation, then isolated bench; test chosen independent guard or one-way wiring | Exact versions/configuration; rejected message classes; no unauthorized state changes; bounded processing and control-link latency |
| Real downlink trust | Map every participant/bridge and inspect signing/encryption/source policies | Injection/replay rejection, route behavior, session/reboot handling, reporter attribution |
| Build and physical access controls | Spare-board provisioning/recovery exercises and read-only eFuse inventory | Signed boot enforcement, debug/download state, approved recovery, unique key inventory |
| Host resource budgets | Sustained valid-input floods and HTTP concurrency/resource tests | Declared CPU/memory/latency limits and fairness; schema rejection/recovery is already verified separately |
| UI content handling | Browser tests using hostile strings and malformed device data | Text-only display, bounded rendering, no script execution or unsafe export interpretation |
| Electrical containment | Non-flying bench with actual board revision and peripheral load | Supply margin; behavior under module reset, disconnect, short/current limiting and startup UART output |
| Dependency exposure | Pin SDK commit and radio-library versions; review relevant upstream advisories | Per-advisory applicability and remediation decision, not a generic ESP32 vulnerability list |

No hardware RF attack, live serial injection, port scan, eFuse operation, FC
configuration change or flash was part of the original baseline review. Later
closure work and its narrower scope are recorded below. No radio remote-code-
execution vulnerability or successful FC takeover was demonstrated. The absence
of such a demonstration does not establish that those paths are impossible.

## Decisions needed for the next implementation milestone

1. Exact FC firmware/target/version and installed port/downlink configuration.
2. **Supported alternatives: second-XIAO guard OR standalone FC UART4 filter.**
   Choose the harness and firmware for the deployed option. Verify the FC route
   and power arrangement. Test direct adversarial input to the selected boundary;
   if combining both, exercise each independently too.
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

## Multi-model guard provisioning

The initial guard 0.2.0 builds for XIAO C5, C6 and S3 exclude radio/network components and
include a fail-closed startup policy smoke test. Web provisioning checks selected
profile, local build shape, immutable image fingerprint and fresh post-flash
model/version/hash/self-test status. These checks are not device attestation.
The optional two-adapter UART fixture tests benign report/heartbeat forwarding,
forbidden parameter requests and bad CRC, then checks diagnostic counter deltas.

See [guard-bench evidence](evidence/guard-bench.json) and the
[fixture guide](../guard/README.md#web-bench-flash-and-test-guards). Browser tests
use simulated devices; UART fixture code is tested with simulated serial ports.
That validation used no device flashing, physical UART, FC, reset, power or RF test.

## P0 closure follow-up

The [closure progress record](evidence/p0-closure-progress.json) supersedes guard
0.2.0 as the current candidate. A read-only USB diagnostic from a connected C5
showed an incorrect factory identity. Source inspection confirmed that the SDK's
generic helper writes eight bytes on IEEE 802.15.4 targets into the guard's
six-byte buffer. Guard 0.2.1 uses the explicit six-byte factory-MAC API. C5/C6/S3
build verification passes; the bench rejects old local builds and keeps guard
protection when identity validation fails. This is a startup defect, not a
demonstrated network exploit.

After the fix, **42 Python tests and six C/C++ sanitizer executables pass**;
desktop/mobile browser checks with simulated devices also pass. See the
[host log](evidence/p0-closure-host-tests.txt).

After the user confirmed the dedicated guard and that the FC/harness is not
ready, the web bench installed 0.2.1 on C5 `38:44:BE:BE:0C:20`. Esptool verified
the written images. The USB check captured two samples in one boot session,
uptime increasing from 5 to 2011 ms, with matching factory identity and local
build hash, `ready=true`, `policy_selftest=true` and `radios_enabled=false`.
Both Scouts retained their sessions and advancing uptime. The
[installation log and samples](evidence/p0-guard-usb-install.json) record this
limited check. It is not device attestation, RF measurement or physical UART,
FC, reset-fault or power-fault acceptance.

The [native FC simulator result](evidence/p0-sitl/result.json) passes for the
standalone ArduPilot route, including matching ID/hello/RSSI delivery, trusted
operator positive controls, denied-class injection with three source-ID pairs,
nested/malformed frames, short line-rate-paced floods and recovery. Checked
state is mode, armed flag, stored mission and the `WPNAV_SPEED` sentinel. RC and
failsafe effects and all-parameter invariance were not asserted. Compressed
both-direction captures, simulator/build logs and hashes accompany the result.

Native FC simulation is reproducible with
[`fc_filter/test_sitl.py`](../fc_filter/test_sitl.py); see its
[run instructions](../fc_filter/README.md#native-ardupilot-simulator-check).
It creates its own disarmed FC process and loopback UART sockets and retains a
failed result on test failure. Hardware acceptance remains governed by the
[P0 checklist](p0-acceptance.md).

During harness development, this pinned SITL release's advertised `--serial9`
option overwrote storage beyond its nine-entry serial-path array, causing
startup/shutdown failures. The runner now specifies only paths 0–8 and requires
a clean exit; the recorded passing run exited with code 0. It also waits for
`ArduPilot Ready` before mission setup. Earlier setup failures are not acceptance
passes and did not involve physical hardware or changes to the FC policy.

## P1 USB validation and reader isolation

**Completed 2026-10-24 for the reviewed schema-1 USB input path.**
[`usb_records.py`](../tools/usb_records.py) validates field shapes/ranges and
bounded structure for Scout/guard status, replies and observations. Both the
stream decoder and state boundary enforce it. The latter copies accepted input
and prepares serialization before changing session/status/contact/log state.
Unknown fields, invalid UTF-8, duplicate keys, non-finite numbers and excessive
size/depth are rejected. See the [host contract](../docs/usb-bench.md#host-validation).

The [P1 evidence record](evidence/p1-usb-validation.json) contains source hashes,
48 passing Python tests, six passing C/C++ sanitizer executables, browser checks,
read-only compatibility validation of 1,503 existing records, and fresh normal
status after the host-service reload on two Scouts (0.5.2) and the guard (0.2.1).
The tests use malformed input followed by valid records, check that rejected
new-session input cannot mutate stored state, and exercise two actual reader
threads plus the real `/api/state` handler with mocked serial. An injected
processor exception proves the port is released, the reader recovers, and the
failure is reported. Browser tests verify diagnostic text remains text, including HTML
markup supplied in the mocked error field.

Reproduce the host checks from the repository root:

```sh
PATH="$PWD/.venv/bin:$PATH" bash tests/run_host_tests.sh
.venv/bin/python threatmodel/checks/host_inputs.py
```

[Host output](evidence/p1-usb-host-tests.txt) and
[boundary probe output](evidence/p1-host-input-probes.jsonl) preserve the results.
The rotating-source/contact-eviction probe still demonstrates an open RF/report
availability issue; it is not silently relabeled as passing containment.
Malformed-input testing used simulated USB streams; physical boards only supplied
normal records. No firmware change or hardware attack injection was needed.
Per-record bounds and reader recovery do not qualify sustained ingress/HTTP
resource limits, source authentication or the deferred P0 hardware gates.
