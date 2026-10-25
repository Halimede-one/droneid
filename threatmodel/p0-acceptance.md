# P0 closure checklist

**Status as of 2026-10-24: partially closed — pending full validation.** The user
accepted partial closure of implementation and the recorded software/USB checks;
full deployment acceptance is deferred. Resume the
[P0 verification plan](p0-verification-plan.md) and enter the later results here.
The independent boundaries are implemented, but physical UART/FC, flight-loop timing and power
acceptance are pending. A connected C5 guard running 0.2.0 failed identity checks;
review found an eight-byte SDK write into a six-byte startup buffer. Guard 0.2.1
fixes it and builds for C5/C6/S3. The user-designated C5 guard
`38:44:BE:BE:0C:20` now runs 0.2.1: flash verification and two fresh USB samples
passed with the correct identity, build hash and startup self-test. See
[installation evidence](evidence/p0-guard-usb-install.json) and
[closure progress](evidence/p0-closure-progress.json). The user confirmed the
FC/harness is not ready; physical acceptance remains pending.

The [hardening work queue](hardening-plan.md#p0-work-queue-and-exit-criteria)
separates code/build completion from deployment acceptance. Prior
[role-workflow verification](evidence/device-role-settings.json) includes mocked
desktop/mobile browser checks; these do not qualify hardware behavior.

**Latest verification:** 42 Python tests, six C/C++ sanitizer executables and
desktop/mobile browser checks pass. A native **ArduCopter 4.6.3 SITL** run also
passes with a clean simulator exit; [result and capture hashes](evidence/p0-sitl/result.json).
On the standalone FC-filter route, it delivered matching ID/hello/RSSI records,
exercised nine denied message classes under three claimed source IDs, kept mode,
armed flag, a stored mission and the `WPNAV_SPEED` sentinel unchanged, contained
a nested command and malformed frames, and recovered after two three-second
115200-baud-paced floods. Trusted-port mode/parameter/mission controls passed.
This is disarmed simulation on unsigned loopback links; RC/failsafe authority,
all-parameter invariance, real UART interrupt load, sustained vehicle timing,
external-guard hardware and signed downlink behavior remain unqualified.

Use **one** of these independent boundaries for a deployment:

- Scout → second XIAO Guard → stock FC, or
- Scout → FC running the custom UART4 ingress filter.

Combining them is optional. Closure applies to the selected hardware, firmware
and wiring. Passing one deployment does not qualify all board variants.

## What is implemented

| Requirement | Status / evidence |
| --- | --- |
| Separate guard with no radio/network stack | C5/C6/S3 guard 0.2.1 builds; [current build and fix evidence](evidence/p0-closure-progress.json). Older 0.2.0 images are not acceptable for installation |
| Independent FC-side filter, usable without a second XIAO | Pinned ArduCopter 4.6.3 and iNav 9.0.0 builds; [FC evidence](evidence/p0-fc-standalone.json) |
| Frame/CRC/schema/source/destination checks; command rejection | Portable policy and adapter tests, including direct Scout and guard input |
| Prevent nested MAVLink commands reaching native FC parsing | Complete-frame quarantine/normalization and DRG2 encoding tests |
| Bound parser work, queues, report rates and output bytes | Enforced limits and offline saturation/recovery tests; actual UART interrupt/control-loop cost remains unmeasured |
| Base-station decoding of guarded reports | DRG2 decoding and record-equality tests; real downlink remains unverified |
| Maintenance workflow | Explicit Scout/Guard firmware installation, per-device logs, USB/startup check and optional two-adapter UART smoke test |
| Designated guard installation and USB startup | C5 ending `BE:0C:20`: 0.2.1 flashed and checked; both Scouts kept their running sessions. [USB evidence](evidence/p0-guard-usb-install.json); physical UART/FC/power not tested |
| Native FC path in simulation | ArduPilot standalone-filter behavior checks and clean exit pass; [captures and limits](evidence/p0-sitl/result.json). Does not close the installed-FC gate |

The [hardening plan](hardening-plan.md#p0--contain-the-flight-controller-boundary)
defines the security requirement: compromised-Scout traffic must not gain flight
command authority or exhaust FC communication/processing budgets.

## Evidence needed to close P0 for the first installation

The [verification plan](p0-verification-plan.md) supplies setup, limits,
procedures PV-01–PV-08 and the evidence/review format for these remaining gates.
All **installation acceptance** gates below remain open. Guard identity and build
are recorded; the FC/harness is not ready. Record actual results and attach
captures; do not check them off from a compile, screenshot, simulator result or
guard-reported status alone. Identify one installation before starting; accepting
one route does not qualify the other route or a different board variant.

| Gate | Required result | Record |
| --- | --- | --- |
| Exact installation | Identify board revisions, selected boundary, installed image hashes, exported FC settings, UART/downlink assignment and signing policy | Guard USB identity/build recorded; FC configuration, full board revisions and harness photographs pending |
| Physical UART path | Verify crossed TX/RX, common ground, idle voltage and startup/ROM behavior. For external guard, no direct Scout-to-FC signal bypass | Wiring inspection; two-adapter smoke-test output where applicable |
| FC authority isolation | In matching simulation, then an isolated non-flying bench, inject permitted reports plus denied classes, malformed/nested frames, forged IDs and replays. Only permitted records reach the report path; mode, mission, parameters, RC/control and failsafe authority do not change from hostile input | Before/after state snapshots, both-side UART captures and FC logs |
| Work and latency bounds | Declare acceptable control-loop/downlink latency and watchdog limits **before** measuring. Flood both valid and rejected input at line rate; maintain those limits and bounded storage; recover when input stops | Rates, durations, maxima, drop counters and scheduler/latency traces |
| Failure behavior | Reset/disconnect/reconnect Scout and guard, interrupt frames and exercise stuck RX and loss of guard power. No unintended command or unsafe startup output; expected communications recover | Reset causes, logic-analyzer captures, recovery times and FC behavior |
| Actual report delivery | Confirm the installed base station receives and decodes the allowed ID/message/RSSI records through the selected FC/downlink, under the intended signing/authentication settings | Captures and decoded records matching the source fixtures |
| Electrical containment (TM-11) | Verify supply margin, power sequencing and current limiting under peripheral load and module fault conditions; FC must remain within its defined supply/reset limits | Measured voltage/current traces and power design |

For the FC-only option, UART interrupt/DMA load reaches the controller before
software filtering. For the external guard, its output limits still require
measurement on the real FC. Neither option alone isolates a shared power fault.

## Remaining functional gap: iNav forwarding

The current iNav filter validates/counts and consumes reports. **It does not
forward those observations to the base station.** If iNav is the first deployment,
implement and test a forwarding path or use a separate gateway before claiming
the full reporting use case works. ArduPilot's report-routing code is present,
but its installed downlink still requires verification.

## Outside this P0 closure

Aircraft identity authentication, reporter authentication/replay policy,
secure-boot/provisioning, broader host hardening and RF robustness remain P1 or
ongoing work. P0 completion would establish the tested containment boundary;
it would not establish trustworthy aircraft identities or resistance to RF jamming.
