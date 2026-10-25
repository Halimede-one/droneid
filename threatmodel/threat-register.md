# Threat register

Review date: **2026-10-24**. Findings describe the
[recorded development baseline](evidence/baseline.json), not an attestation of
plugged-in devices. **Observed** means a local offline probe reproduced the
behavior; **code-supported** means a source/configuration fact;
**conditional** means exploitation requires an additional capability or an
unproven defect. Impact is qualitative, not a CVSS score or estimated likelihood.
Mitigation implementation and deployment acceptance are separate states. A
compiled or simulator-tested control does not close the installed-hardware risk.

**P0 disposition, 2026-10-24: partially closed — pending full validation, at the
user's direction.** The implementation and recorded software/USB verification
portion is closed for this milestone. TM-04 retains pending installed-boundary
acceptance; TM-11 remains open for electrical validation. Severity and priority
are unchanged. Resume with the [P0 verification plan](p0-verification-plan.md);
this decision does not claim a flight-qualified installation.

**P0 follow-up:** an [independent guard](../guard/README.md) now implements the
TM-04 protocol restriction outside the radio sensor. Alternatively, a standalone
[FC ingress filter](../fc_filter/README.md) provides pinned ArduPilot/iNav
patches before normal parsing/routing and accepts a single radio XIAO directly.
Combining both is optional. Build and offline results are recorded
in [FC evidence](evidence/p0-fc-standalone.json). The latest
[closure progress](evidence/p0-closure-progress.json) includes a guard startup
memory fix and rebuilt 0.2.1 images. Installed-FC and electrical acceptance are
still open; the [closure checklist](p0-acceptance.md) is the release gate.
The baseline findings below remain applicable to the direct sensor connection.

| ID | Threat | Impact | Evidence / condition | Priority |
| --- | --- | --- | --- | --- |
| TM-01 | Forged or replayed aircraft claims | High for operator decisions | Code-supported; no identity authentication | P1 |
| TM-02 | RF/processing flood and loss of legitimate contacts | High for receiver availability | Contact eviction observed; RF saturation untested | P1 |
| TM-03 | Radio parser/stack compromise | High; enables TM-04 | Conditional on a reachable vulnerability; none demonstrated | P1 |
| TM-04 | Compromised ESP32 attacks FC over UART | Potentially critical for flight safety | Implementation/recorded checks closed; independent guard OR FC filter available; full FC/harness validation deferred | P0 — partially closed |
| TM-05 | Spoofed or replayed FC telemetry into ESP32 | Medium now; higher if used for transmission/control later | Unsigned replay observed | P1 |
| TM-06 | Forged reports or hostile content reach base station | High for evidence integrity | Code-supported; requires report-path access or compromised reporter | P1 |
| TM-07 | Malformed USB status breaks monitoring | Medium for bench availability | USB schemas, pre-commit validation and per-device reader recovery implemented and verified | P1 — closed for reviewed input path |
| TM-08 | Unauthorized USB/debug/configuration access | High; can enable TM-04 | Development interfaces and build settings; physical eFuses unverified | P1 |
| TM-09 | Local process abuses web-bench authority | High with maintenance privileges | Code-supported local trust assumption; conventional browser attacks guarded | P1 |
| TM-10 | Modified firmware/dependency supply chain | High; can enable TM-04 | Artifact hashes are not signatures; no secure boot in reviewed build | P1 |
| TM-11 | Shared power fault affects FC | Potentially critical for flight safety | No measured power containment; neither software policy addresses it | P0 — open electrical gate |

P0 means resolve the architecture or integration risk before relying on an
FC-connected installation in adversarial conditions. P1 means harden the next
security milestone. Ranking does not imply an exploit has been found.

## TM-01 — Forged and replayed aircraft claims

**Path:** attacker broadcast → radio parser → contact display/log → operator.
An attacker can claim another aircraft's ID, replay an earlier hello or position,
or create many identities. A source MAC is also a claim. Copying a legitimate
MAC can merge attacker messages with that contact; rotating MACs can split one
transmitter into many contacts. Successful decoding and a plausible ID do not
establish ownership or friendly status.

The receiver records its measured RSSI and local receive time, but neither
authenticates the aircraft. Transmit power, relays, antennas and geometry affect
RSSI. A replay received now has a new local timestamp; field-age tracking does
not establish when the claimed event actually occurred. Public ID/hello traffic
also permits observation and tracking; do not put secrets in those messages.

**Existing controls:** [RID serialization](../components/rid/rid.c) explicitly
marks authentication `unverified`; [contact aggregation](../tools/dashboard.py)
keeps separate field ages, expires contacts and clears a hello when an ID changes.
Authentication-type RID records are decoded as data, not cryptographically
verified. The need to distinguish broadcast claims from authenticated identity
is also discussed in the [DRIP architecture, RFC 9434](https://www.rfc-editor.org/rfc/rfc9434.html).

**Gap / action:** make “claimed / unverified” visible beside identity, preserve
source/receiver/time provenance, and never use the ID or RSSI alone to authorize
flight action. A future trusted-fleet feature needs a reviewed authentication,
freshness and key-revocation design. A secret-looking hello string is public
and replayable. Deduplication can reduce noise but cannot authenticate a sender.

## TM-02 — Reception and resource denial of service

**Path:** airtime interference or high-rate broadcasts → radio/controller work →
queues, contacts, logs and downlink contention. Jamming can prevent reception
regardless of packet validation. Well-formed floods can consume resources without
any memory-safety defect. MAC rotation defeats a per-MAC quota by itself.

**Existing controls:** [Wi-Fi framing](../components/rid/rid_wifi.c) bounds packs
to 1–9 messages and checks lengths before copying. Submission uses nonblocking
queues: USB 64 observations and UART 32. The [UART worker](../main/fc_uart.c)
expires queued reports after two seconds, sends at most the configured 1–50
observations/s (default 5), prioritizes heartbeat/health and bounds each loop.
The dashboard keeps 128 contacts, 500 in-memory log records per device and
rotating disk logs. These controls bound growth; they do not reserve capacity
for genuine traffic. The contact-eviction probe demonstrates this distinction.

**Gap / action:** there is no global ingress work budget or fairness mechanism
before all parsing/callback work. Add overload counters and explicit degraded
reception indicators; use a total budget as well as bounded per-source accounting.
Test sustained valid and malformed traffic on hardware with recovery measurements.
The [interleaved scan schedule](../main/radio.c) already has normal blind periods
and limited channel coverage; silence must not be presented as proof of clear air.
Host fuzz tests do not measure RF starvation, watchdog behavior or FC latency.

## TM-03 — Radio parser or stack compromise

**Path:** unauthenticated RF input → vendor controller/driver or application
parser bug → crash, memory corruption or code execution. This is a plausible
attack class, **not a demonstrated vulnerability in this build**.

The [Wi-Fi implementation](../main/wifi_radio.c) starts STA mode for capture/raw
transmission without associating, creating a default network interface, or
opening application IP services. `DroneID-BENCH` is a raw beacon label, not an
application access point. [BLE](../main/ble_radio.c) uses passive discovery and
nonconnectable advertising. Central/peripheral roles are disabled. There is no
application GATT, pairing workflow, network provisioning or OTA service.

Those choices remove ordinary login/service paths, but both radios process
untrusted traffic before the application can reject it. Malformed-input host
tests cover portable C code, not the radio controller, binary Wi-Fi libraries,
interrupt paths, coexistence timing or every upstream codec behavior.

**Existing controls:** bounds/length checks, fixed-size copies, bounded JSON
formatting, ASan/UBSan host tests, task stack canaries and ESP system memory
protection. **Gaps:** the recorded build has compiler stack protection disabled;
system memory protection is not evidence of isolation between radio and UART
tasks. Review production compiler settings, use coverage-guided parser fuzzing,
record exact SDK/component revisions and triage applicable advisories.
The [Espressif advisory index](https://github.com/espressif/esp-idf/security/advisories)
is an input to that process, not evidence that every ESP32 advisory affects C5
or this observer-only BLE configuration. A complete version-to-advisory audit
was not performed.

## TM-04 — Compromised ESP32 pivots into the flight controller

**Path:** TM-03, TM-08 or TM-10 → attacker-controlled ESP32 → physical UART TX →
FC MAVLink handlers/routing → possible commands, mission/configuration changes,
telemetry spoofing or resource exhaustion. The impact depends on the installed
FC's acceptance rules and current state. No actual FC command execution or
in-flight effect was demonstrated.

**Normal application behavior:** [fc_link.c](../components/fc_link/fc_link.c)
emits only a component `HEARTBEAT` and `TUNNEL` observation/health frames.
Source/destination IDs come from saved configuration, not the received RID.
The 25 raw RID bytes remain opaque payload; they are not fed to a command parser.
The offline probe embeds a complete benign heartbeat and confirms one outer
TUNNEL frame with the original bytes intact. The worker's rate cap bounds normal
RF-induced report traffic. This is evidence against a direct hello-to-command
injection path, not a guarantee against corrupted serial framing or unknown bugs.

The P0 tests additionally demonstrate that a fresh parser starting inside an
unguarded binary payload can find the embedded frame. The guard's DRG2 encoding
and regenerated header/CRC remove all interior MAVLink start bytes from its
FC-bound output. Restart-at-every-byte tests cover this specific framing gap;
they do not establish immunity to electrical injection or downstream bugs.

**After compromise:** the attacker can replace those behaviors, spoof MAVLink
IDs, bypass rate limits and transmit other messages. Neither the module's
component ID nor its intended role limits physical TX authority. Disabling UART
in the same firmware or adding an ESP-only allowlist cannot contain its takeover.

**Controller examples, not deployed-controller results:**

- ArduPilot's [routing design](https://ardupilot.org/dev/docs/mavlink-routing-in-ardupilot.html)
  separates message handling/forwarding by targets and learned routes. The
  [Copter 4.6.3 `accept_packet` implementation](https://github.com/ArduPilot/ardupilot/blob/Copter-4.6.3/libraries/GCS_MAVLink/GCS_Common.cpp)
  accepts packets claiming the vehicle's own system ID at that check. An ID
  setting is not a secret or a per-port permission. Other signature/command
  checks still matter; this is not a claim that every command is accepted.
- [iNav 9.0.0 MAVLink telemetry](https://github.com/iNavFlight/inav/blob/9.0.0/src/main/telemetry/mavlink.c)
  processes incoming messages, including mission upload/clear handlers,
  `COMMAND_INT` reposition handling with checks, and RC override dispatch whose
  effect depends on receiver configuration. “Telemetry Output,” lack of TUNNEL
  forwarding, or half-duplex pacing does not establish a read-only connection.

**Mitigation status: partially closed — implementation and recorded checks
complete; full deployment validation deferred.** A separate
C5/C6/S3 guard or the pinned standalone FC UART4 filter enforces schema,
command rejection, normalization and byte/work limits outside the Scout's trust
domain. Host sanitizer and adapter tests pass. This does not qualify UART
interrupt load, armed/flight behavior, downstream routing or reset behavior on
the installed controller. Direct Scout wiring to a stock FC remains uncontained.

The [native ArduPilot simulator run](evidence/p0-sitl/result.json) passes on the
standalone-filter route: reports decode at the operator, mode/armed flag/mission/
test parameter remain unchanged under the recorded attacks, short floods recover,
and the simulator exits cleanly. This is partial TM-04 evidence: unsigned,
disarmed SITL does not establish RC/failsafe authority, signed routing or physical
FC processing limits. No installed vehicle risk is downgraded from this result.

**Guard defect found during closure review:** guard 0.2.0 called
`esp_efuse_mac_get_default()` with a six-byte buffer. The reviewed ESP-IDF helper
writes eight bytes on C5/C6, causing a startup buffer overwrite and incorrect
identity. A connected C5 reported `3844befeffbe` instead of USB identity
`3844bebe0c20`; its bench check failed. No attacker-controlled trigger or remote
exploit was demonstrated. Guard 0.2.1 explicitly requests the six-byte factory
MAC, and the bench rejects older local builds and preserves guard protection
when status identity is invalid. All three supported profiles rebuild. The
designated C5 `38:44:BE:BE:0C:20` has now been flashed to 0.2.1 and passed two
fresh status checks with matching identity/build hash and startup self-test.
This closes the startup-fix installation item on that guard, not TM-04's physical
containment gate. See [installation evidence](evidence/p0-guard-usb-install.json).

**Next action:** prepare the FC/harness, which the user confirmed is not ready;
execute the physical acceptance gates and record the exact FC build,
configuration and wiring. The standalone FC-filter route remains an independent
alternative. Resume cases PV-01–PV-06 and PV-08 in the
[verification plan](p0-verification-plan.md). See
[P0 architecture and work queue](hardening-plan.md#p0--contain-the-flight-controller-boundary).
MAVLink signing can authenticate key possession and provide replay protection;
it does not itself define a sensor-only permission. Giving the ESP32 a key that
authorizes general FC commands can preserve the pivot after compromise.
[MAVLink signing specification](https://mavlink.io/en/guide/message_signing.html).

## TM-05 — Forged controller telemetry into ESP32

**Path:** attacker with UART access, or a participant whose traffic the FC routes
onto that UART → matching source IDs → ESP32 diagnostic telemetry. This is not a
direct Remote ID radio path.

[`fc_receive`](../components/fc_link/fc_link.c) checks CRC, configured system and
component IDs, heartbeat type and some coordinate ranges. It ignores signed
frames because verification/key provisioning is absent. It accepts unsigned
frames, with no authenticated source or replay check. The probe replays the
identical valid heartbeat after expiry and observes a refreshed heartbeat age.

**Impact:** false connection/mode/position diagnostics now. The incoming
position does **not** currently feed own-aircraft RID broadcasts. If that changes,
this threat can contaminate transmitted aircraft data and must be reassessed.

**Action:** implement source authentication and replay policy on the chosen
integration path before using telemetry as authoritative aircraft data. Retain
age/range checks, but distinguish receive freshness from source authenticity.
Do not disable a vehicle's existing signing requirements merely to connect this
unsigned development module.

## TM-06 — Forged reports or hostile data at the base station

**Path:** compromised reporter, downlink injector or reachable UDP sender →
custom base-station parser → operator records. A genuine RF receiver can also
faithfully relay false aircraft claims: reporter trust and aircraft trust are
different properties.

[The supplied decoder](../tools/mavlink_receiver.py) checks TUNNEL type/target,
DRID magic/version, exact record sizes and metadata ranges. It does not enforce
an expected source identity, verify a signing key, or reject report replays.
Receiver ID, boot session and sequence are bytes in an unsigned report, not
attestation. Network exposure depends on the chosen `--connect` endpoint and
the deployment's downlink, which were not examined.

**Existing controls:** explicit binary parsing and JSON serialization; the
[local UI](../web/app.js) escapes claimed identity/hello text or uses text nodes.
No radio-origin script or shell execution path was found there. This does not
establish the security of future custom operator code.

**Action:** authenticate the report transport/reporter independently of RID
identity, track replay/session state, cap aggregate work and reject invalid
records. Add separate fields for reporter authentication, claimed-aircraft
authentication and freshness. Keep text untrusted through HTML, terminal,
database and export consumers; never reinterpret raw RID/TUNNEL payload as
commands. Logs currently provide neither tamper-evident storage nor signed
provenance.

## TM-07 — Malformed USB record breaks bench monitoring

**Status: closed for the reviewed malformed-USB input path, 2026-10-24.**
The [P1 verification record](evidence/p1-usb-validation.json) covers strict schemas,
state isolation, recovery and multi-device HTTP availability. Sustained resource
floods and broader HTTP limits remain separate open P1 items; this closure does
not establish authenticity of device claims.

**Original defect (now fixed):** a status JSON value `1e309` becomes Python infinity in
[`LineDecoder`](../tools/bench.py). `parse_constant` rejects literal `NaN` and
`Infinity`, but not this numeric overflow. The decoder accepts the status.
[`Device.record`](../tools/dashboard.py) stores it in status/log state before
`json.dumps(..., allow_nan=False)` raises `ValueError`. The reader catches
serial/OS errors, not this exception. The offline fake-USB probe confirms the
exception escapes the actual reader and its snapshot cannot be serialized.
In the threaded service this terminates that reader and can disrupt shared
state responses; this is an availability finding, not code execution.

**Precondition:** a malicious/reflashed USB device or compromised ESP32 can emit
arbitrary JSONL. No path was found for an ordinary radio payload to make the
current firmware emit this malformed status. The USB serial identity comparison
is useful against accidentally selecting another device, not a cryptographic
proof against a malicious endpoint.

**Implemented control:** [`usb_records.py`](../tools/usb_records.py) validates
Scout/guard status, replies, observation metadata/decoded fields and nested UART/
GPS/counters. It enforces field types/ranges, required and allowed keys, finite
numbers and bounded structure. `LineDecoder` rejects duplicate keys, invalid
UTF-8 and over-limit/deep input and resynchronizes at a newline. `Device.record`
revalidates/copies and prepares JSON serialization before mutating device state.
Reader exceptions produce bounded per-device diagnostics and release/retry that
port. Rejection/retry counters remain visible after a fresh status recovers.

**Guard-bench update:** `LineDecoder` now rejects non-finite parsed floats before
state mutation, including exponent overflow. The offline probe now verifies the
real reader recovers and produces a serializable snapshot after a subsequent
valid record. Guard diagnostic fields also receive strict type/range checks.
The subsequent P1 regression covers malformed nested/type/range cases, invalid
new-session records leaving status/contacts/logs unchanged, healthy-device
continuity and actual `/api/state` handler responses while the bad reader recovers.
The [baseline evidence](evidence/boundary-checks.jsonl) is retained as history.

## TM-08 — USB maintenance, debug and physical access

**Path:** unauthorized USB host or physical access → configuration commands,
ROM flashing/debug → identity/role/UART changes or firmware replacement.
[`app_main.c`](../main/app_main.c) accepts USB configuration commands without
authentication; request tokens correlate replies rather than authorize callers.
The development workflow deliberately keeps recovery/flashing available.

**Existing controls:** bounded command lines, strict identity/hello/pin/ID/rate
validation, separate persisted UART settings and disabled-by-default UART.
These prevent some mistakes and malformed input, not an authorized-looking
malicious change.

**Build evidence:** secure boot, flash encryption and NVS encryption are not
enabled in the reviewed [configuration snapshot](evidence/baseline.json).
USB Serial/JTAG is selected. `CONFIG_SECURE_ROM_DL_MODE_ENABLED=y` alone does
**not** prove ROM/debug restrictions were burned into a particular device's
eFuses. No eFuse state was read during this review.

**Action:** design separate development and deployment provisioning policies,
physical maintenance access and per-device inventory; test signed boot/recovery
and debug/download restrictions on spare boards before committing irreversible
eFuses. Flash encryption addresses stored-data exposure; secure boot addresses
firmware authenticity. Neither prevents RF spoofing, jamming or all runtime
exploits. [ESP32-C5 security overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/security/security.html).

## TM-09 — Web bench trusts the local host

**Path:** local process → loopback API → device configuration/flashing, or
resource exhaustion. [`server.py`](../tools/server.py) binds to `127.0.0.1`,
checks Host and Origin, requires a random token for mutations, limits POST
bodies, restricts static paths and sets a restrictive CSP. These are useful
controls against ordinary cross-origin browser requests and DNS rebinding.

The same service returns the mutation token to an allowed local GET. A local
process can set Host and omit/forge Origin; those headers are not credentials.
The service does not distinguish operating-system users sharing its loopback
network namespace. It has no independent user/device authorization layer.
Threaded HTTP requests also lack a general connection/concurrency budget;
slow-reader/connection exhaustion is plausible but was not stress-tested.

**Action:** keep the bench on a trusted maintenance workstation, do not expose
it through a LAN bind or reverse proxy, and use OS isolation/private IPC if
untrusted local users or programs are in scope. Preserve browser protections;
add request timeouts/concurrency limits and explicit maintenance authorization
before adding remote access. A future remote bench would require a new review.

## TM-10 — Firmware and dependency provenance

**Path:** modified build/toolchain/dependency → locally accepted flash bundle →
attacker firmware → TM-04 or falsified reports. There is no network OTA path,
but USB flashing still imports executable code.

[`Firmware.bundle`](../tools/dashboard.py) checks chip, project name, layout,
file containment and partition sizes. It fingerprints the bytes and the flash
job uses a frozen bundle. This prevents accidental mismatch or a stale UI
selection, but an attacker who can replace the local build can create another
valid-looking bundle. Hash equality proves consistency, not an authorized signer.

**Action:** pin and inventory SDK/codec/MAVLink/host dependencies, verify upstream
advisories against the actual features and versions, record reproducible build
inputs and sign approved release artifacts. Protect signing keys and connect
the artifact policy to secure boot. Avoid treating a reported firmware version
or a USB serial number as proof of what code is running.

## TM-11 — Shared power and electrical fault propagation

**Mitigation status: open; hardware evidence required.** No measurements justify
lowering this risk or closing it with TM-04's protocol implementation.
It remains a deferred validation item within the partially closed P0 milestone;
run PV-07 and review its evidence under PV-08 in the
[verification plan](p0-verification-plan.md).

**Path:** short, wiring error, peripheral current draw or deliberate electrical
fault → shared supply/ground disturbance → FC reset or degraded operation.
The example harness shares ground and can power the ESP32 from the FC's 5 V
rail. Software packet filtering does not address this path.

**Existing controls:** [wiring guidance](../README.md#flight-controller-wiring-and-uart-setup)
separates USB bench power from optional vehicle power and specifies 3.3 V UART
logic. Actual rail headroom, current limiting, startup/reset behavior and fault
isolation have not been measured on this board revision.

**Action / closure evidence:** record the installed power design and declare
permitted FC rail voltage, reset and recovery limits before testing. Verify peak
load and brownout margin with all peripherals attached;
choose independent regulation/current limiting where needed and test module
disconnect/reset/fault behavior on a non-flying bench. Do not infer containment
from a nominal supply rating or from a UART-only protocol guard. Attach voltage/
current traces, observed FC reset causes and pass/fail results to the
[electrical acceptance gate](p0-acceptance.md#evidence-needed-to-close-p0-for-the-first-installation).
