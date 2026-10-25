# P0 verification plan — resume after bench setup

**Decision, 2026-10-24: P0 is partially closed, pending full validation.**
Implementation and the recorded software/USB checks are complete for this
milestone. Installed-FC, UART, timing, failure and electrical acceptance remain
unfinished. This plan preserves that work for a later session; it records no new
test passes and does not qualify an installation for flight.

Use the [acceptance checklist](p0-acceptance.md) for current results and this
document for the procedure. The [register](threat-register.md) tracks TM-04
(UART authority/resource containment) and TM-11 (electrical fault propagation).
TM-04 is partially closed at implementation level; TM-11 remains open.

## Resume here

1. Confirm the MicoAir H743 V2 AIO and harness are ready on a non-flying bench.
   Record actual board revisions, FC firmware/version and the operator downlink.
2. Select one boundary: **external guard OR custom FC UART4 filter**. The current
   designated guard is the XIAO C5 ending `BE:0C:20`; the alternatives do not
   require each other. For a combined installation, test each boundary
   independently as well as the complete chain.
3. Recheck device identity and image hashes against the saved evidence. Restore
   the local tool environment if needed; do not assume old `/tmp` checkouts or
   device names such as `/dev/ttyACM2` still identify the same hardware.
4. Assign a test operator and a result reviewer, create a new evidence directory,
   and fill the limits table below **before** running acceptance tests.
5. Execute PV-01 through PV-08 in order. A failed prerequisite stops dependent
   tests. Record failed and unrun cases; fix a failure and repeat affected cases
   without overwriting the original evidence.

## Last verified state

| Item | Recorded baseline and evidence |
| --- | --- |
| Intended FC | MicoAir H743 V2 AIO; installed firmware/settings and PCB revision still unrecorded; user confirmed FC/harness not ready |
| Dedicated guard | XIAO ESP32-C5, USB `38:44:BE:BE:0C:20`, receiver `3844bebe0c20`, guard **0.2.1** |
| Guard application ELF hash | `2f8ba351fccf70d43090420621344a6cb02561c4684d4251d47ec3d588b648dc`; full image-bundle fingerprint in [installation evidence](evidence/p0-guard-usb-install.json) |
| Guard USB verification | Esptool image verification and two fresh matching status samples passed; startup self-test/readiness passed; radios reported disabled; this is not attestation or an RF measurement |
| Other USB boards | Scouts `10:BD:A3:CA:38:60` and `38:44:BE:1F:48:50` last observed on 0.5.2; verify the selected Scout's current settings before testing |
| Portable/software checks | 42 Python tests, six C/C++ sanitizer executables and mocked desktop/mobile browser checks passed; [progress](evidence/p0-closure-progress.json), [host log](evidence/p0-closure-host-tests.txt) |
| Native FC simulation | Pinned ArduCopter 4.6.3 standalone filter passed ID/hello/RSSI delivery, recorded denied-input cases, brief floods and recovery with clean exit; [result/captures](evidence/p0-sitl/result.json) |
| Simulation limits | Disarmed, unsigned loopback links; checks mode, armed flag, mission and one parameter sentinel. No RC/failsafe-effect assertion, all-parameter audit, physical UART/IRQ timing, external-guard hardware or power test |
| Other profiles | C6/S3 guard builds verified, not physically qualified. RP2040/RP2350 are candidates without a firmware port here |
| Equipment | No USB–UART fixture adapters were enumerated; ownership/availability of adapters and measurement equipment is unconfirmed |

Guard 0.2.0 is excluded: it used a six-byte buffer with an SDK helper that writes
eight bytes on C5/C6. The designated C5 was updated and verified; do not restore
the older image when recreating this bench.

## Setup, tools and evidence

Prepare the FC, selected Scout/guard, actual downlink/base-station parser, a
controlled input generator and simultaneous input/output capture. The external
guard smoke fixture needs **two uniquely identified 3.3 V USB–UART adapters**.
Use a logic analyzer for framing/startup timing and voltage/current measurement
equipment with a current-limited supply/electronic load for electrical tests.
Capture the actual flight-controller and peripheral supply rails. Keep motors
disconnected during command/arming tests and record the power arrangement.

Use the [guard wiring guide](../guard/README.md) or
[direct FC-filter wiring guide](../fc_filter/README.md#build-and-install-in-an-fc-source-checkout).
For the C5 guard path, Scout D6 TX → guard D9 RX; guard D8 TX → Scout D7 RX;
guard D4 TX → FC RX4; FC TX4 → guard D5 RX; common GND. Guard D6/D7 remain
unconnected. UART is 115200, 8N1, non-inverted, 3.3 V logic. Inspect for a direct
Scout→FC bypass. Adapter power pins remain disconnected in the USB-powered
fixture. Replace the Scout's TX connection with the test sender when injecting;
do not tie two TX outputs together or rely on normal Scout firmware to generate
the hostile stream.

Create `threatmodel/evidence/p0-validation/<run-id>/` with a unique run ID. Keep
large raw captures outside Git if needed, but record durable locations and hashes.
Use this layout or an equivalent indexed manifest:

```text
run.md                 # operator/reviewer, date, topology, limits and case results
manifest.json          # board IDs, source revisions, image/capture hashes, tool versions
config/                # FC export, Scout/guard settings, signing policy (no private keys)
wiring/                # annotated harness/power diagram and photographs
baseline/              # state, FC logs, timing and power before injection
pv-02/ ... pv-07/       # generator inputs/seeds, captures, decoded output, measurements
decision.md            # full/partial/fail decision, scope and remaining issues
```

Preserve raw bytes in both directions, decoded records, host and device
timestamps, boot sessions, drop/error counters and FC event/scheduler logs.
Declare clock synchronization and measurement resolution; a host timestamp alone
cannot prove sub-millisecond FC timing. Record image hashes separately from
self-reported firmware identity. Do not store signing secrets in evidence.

Existing commands, run from the repository root when reproducing a baseline:

```sh
# Host checks; install the existing project dependencies first if necessary.
PATH="$PWD/.venv/bin:$PATH" bash tests/run_host_tests.sh

# Local guard image checks; build in an activated ESP-IDF environment if absent.
.venv/bin/python guard/verify_build.py --board xiao_esp32c5

# Native FC simulation: requires the pinned patched source checkout and a NEW output directory.
.venv/bin/python -m pip install -r fc_filter/requirements-sitl.txt
.venv/bin/python fc_filter/test_sitl.py \
  --checkout /path/to/patched-ardupilot \
  --output /tmp/droneid-p0-resume-sitl

# Local bench, then Device settings → Guard → Run USB check / Wired UART test.
.venv/bin/python ./start-web
```

Restore the pinned FC checkout using [FC build/install instructions](../fc_filter/README.md).
The simulator runner creates its own process/sockets and accepts no hardware
endpoint. It is **not** the full physical acceptance runner. The web wired test
is a six-second smoke test with the FC and Scout disconnected. Sustained-load,
state-invariance, fault orchestration and power acquisition still need a hardware
test harness/operator procedure; retain its source/version and generated input.
Repeat existing software tests when the candidate/configuration has changed,
or to re-establish a missing baseline; do not treat repetition as new hardware
evidence.

## Limits to declare before testing

Fill every applicable value in `run.md` and identify its source (vehicle
requirement, component rating or agreed test target). An unset limit makes that
case **not validated**. Do not choose a limit after seeing the result.

| Quantity | Value to record | Measurement |
| --- | --- | --- |
| FC loop period/jitter and missed-deadline allowance | Required maximums and permitted misses | FC scheduler traces under normal peripheral load |
| Trusted operator command latency/loss | Maximum round trip, percentile and allowed loss | Timestamped benign commands/acks on the real operator link |
| Observation delivery latency/loss | Nominal rate, maximum age/latency and allowed losses | Match fixture sequence/raw record to base-station receipt |
| Recovery | Maximum time after flood, disconnect, reset and power restoration | Input stop/restoration to fresh permitted report and normal operator control |
| Resources | FC/guard heap floor, queue bound, reset/watchdog allowance | Heap/drop/error counters and FC reset/event logs |
| Electrical | FC rail min/max, ripple/transient duration, peripheral peak/current limit and permitted reset behavior | Instrument traces at the FC and protected peripheral branch |
| Test duration and coverage | Baseline, per-load duration, recovery window and reset/fault cycles | Generator/capture manifest with achieved rate and actual duration |

Planning defaults for the later operator: 10 minutes nominal baseline, at least
15 minutes per sustained-load pattern, and 10 cycles per reset/disconnect case.
These are proposed coverage minimums, not measured results or a flight guarantee;
record any justified change before execution. Include representative peripheral
load and relevant simulated flight/control states, not only an idle disarmed FC.

## PV-01 — Freeze the candidate and inspect the installation

Record topology, hardware revisions/USB IDs, exact source revision plus local
diff, image hashes, SDK/compiler/tool versions, FC parameter/configuration export,
UART mapping and baud, telemetry path and signing policy. Identify which
participant can transmit on each link. Confirm the actual FC image contains the
filter for the FC-only route; a matching UART setting is not proof of filtering.

Recheck the guard's USB status against the local build for the external route.
Inspect the unpowered harness for crossed TX/RX, common ground, supply separation,
one transmitter per wire, and absence of a direct bypass. Measure idle logic
levels, then capture boot/ROM/reset output on the **FC-facing** line. Record the
guard chip revision separately from the PCB revision. Configure the operator
link independently of the Scout port and retain a way to restore the FC settings.

**Pass:** unambiguous candidate/configuration, appropriate physical wiring and
logic levels, completed limits table and usable captures. Otherwise stop before
injection. Identity alone does not pass the other cases.

## PV-02 — External guard fixture (guard route only)

Disconnect Scout and FC; connect the two adapters as described in the
[wired-test instructions](../guard/README.md#web-bench-flash-and-test-guards).
Run the USB check and six-second wired smoke test; save operation output and
status samples/counter deltas. Verify observation forwarding toward the FC side,
permitted FC heartbeat return, rejection of forbidden parameter requests and bad
CRC, and absence of unexpected message classes or interior frame-start markers
in the guard's encoded output. Record UART/framing errors and their cause.

**Pass:** the fixture's checks pass with the expected diagnostic changes and no
unexpected output. This is wiring/policy smoke evidence only. FC-only installations
record this case as **not applicable: no external guard**, then continue.

## PV-03 — Actual report delivery and normal operator control

Connect the selected installation to its real base-station link. Establish route
learning with the intended base-station heartbeat and configured signing policy.
Send known ID, hello, RSSI and health fixtures at nominal rates. Match raw record,
receiver/session/sequence and decoded ID/hello/RSSI at the base; check that bytes
inside a Remote ID record remain observation data. Confirm source/destination
envelopes for this route (defaults: vehicle 1, Scout 242 or guard 243, FC 1,
base 255/190) and permitted reverse heartbeat/position/GPS behavior where used.
Run benign operator-command positive controls through the ordinary operator link.

**Pass:** intended reports arrive and decode within declared limits, and ordinary
operator control still works. Signing-required policies may reject the current
unsigned reports: record that as a delivery/design failure, not a reason to
weaken the operator link. **iNav currently consumes reports without forwarding
them**; an implemented, tested forwarding/gateway path is required to pass this
case on iNav. Existing ArduPilot simulator routing does not qualify a real downlink.

## PV-04 — Authority isolation under hostile UART input

First exercise missing state/failsafe cases in matching simulation, then on the
isolated physical bench. Capture a complete mission, relevant RC/control and
failsafe state, mode/arming state and a full configuration/parameter snapshot.
Declare volatile fields excluded from comparison beforehand. Observe state
continuously as well as before/after, so a transient change cannot be hidden by
a later return to baseline. Establish equivalent benign positive controls through
the trusted operator path where the case permits them.

Inject at the compromised-Scout side of the chosen boundary:

| Input family | Required cases |
| --- | --- |
| Commands/state changes | Mode/arm/disarm, parameter write/read, mission upload/clear, RC override, reposition, reboot and serial-control/passthrough requests; record preconditions for each |
| Claimed authority | Sensor, guard, FC and GCS source IDs; wrong/broadcast destinations; GCS/autopilot-looking heartbeats; MAVLink 1 and signed/unsupported-flag envelopes |
| Framing/schema | Bad CRC, wrong lengths/version/kind, invalid metadata/padding, truncated and concatenated frames, noise and gaps, oversize frames and valid commands nested inside opaque RID data |
| Replay/mixed traffic | Repeat permitted reports and denied commands; interleave legal and illegal input; change IDs/sequence/session; resume with a fresh permitted report |
| Failsafe/RC authority | Loss and restoration of the trusted control link while the Scout continues emitting forged heartbeats/RC traffic; compare with the declared normal failsafe behavior |
| FC-only port restrictions | Attempts to reassign UART4 or enable alternate parsers/passthrough, including after reboot, using the supported maintenance procedure |

For the external guard also exercise the reverse FC→Scout allowlist and verify
position handling requires the intended fresh heartbeat. Replay of an otherwise
valid observation is not necessarily rejected: reporter authentication/replay
protection is P1. It must remain a bounded observation and gain no control authority.

**Pass:** only permitted normalized report/telemetry classes cross the applicable
boundary; hostile input causes no unauthorized mode, arming, mission, parameter,
RC/control or failsafe change. No general passthrough or alternate parser is
reachable on the protected FC port. Normal operator actions still work and
fresh reports recover. A precondition that makes a command impossible is not
by itself proof that the boundary rejected it; use captures/counters and positive
controls. Keep unanswered cases pending.

## PV-05 — Sustained load, work bounds and recovery

Capture the declared nominal baseline with normal peripherals/downlink active.
Then send valid observations/health, denied commands, random/malformed bytes and
mixed traffic as separate patterns at actual UART line rate, including bursts
and alternating idle/flood intervals. At 115200 8N1 the wire carries at most
11,520 data bytes/s; record achieved rate rather than assuming the generator
reached it. Where applicable, load both guard directions concurrently.

Record FC scheduling, trusted-link latency/loss, report age/drops, memory/queue
use and errors during each pattern and after it stops. Correlate captures with
policy counters. Current code admits 4,096 input bytes/s with burst 512 and at
most 128 bytes per receive call; FC-bound policy output/admission is 800 bytes/s
with burst 160. The external guard's reverse budget is 500 bytes/s with burst
160. Observation/health intervals are at least 200 ms/2 s. Recheck these values
against the candidate sources; distinguish admission counters, UART buffering
and on-wire measurements rather than applying the filter limit to all FC traffic.

**Pass:** declared control/timing/memory limits hold, report work is bounded,
no unauthorized command appears, and operation recovers within the declared
time. Expected drops during overload are counted and visible. For FC-only
containment, measure IRQ/DMA/driver cost before the filter; parser budgeting
does not establish a bound on that cost. A heartbeat surviving a short flood
does not pass this sustained vehicle-timing case.

## PV-06 — Reset, disconnect and failure behavior

Test Scout reset/disconnect, external-guard reset/disconnect/power loss where
present, FC restart, startup ordering, interrupted frames, a source that stops
mid-frame, and stuck-high/stuck-low RX using a suitable controlled fixture.
Repeat with an idle link and while hostile traffic is present. Observe the
FC-facing UART through reset/bootloader transitions, not just application logs.
Check stale pending reports, session changes and fresh-report recovery.

**Pass:** no unintended command or programming/banner output enters the FC,
no unauthorized state change occurs, queues/stale data obey the policy, and
recovery meets the declared limit. A guard heartbeat must not be mistaken for
proof that the Scout is healthy. USB-open/restart behavior alone does not qualify
ROM pins, independent power loss or an installed UART reset case.

## PV-07 — Electrical fault containment (TM-11)

Use the intended regulation/current-limiting topology and representative maximum
peripheral load. Measure at the FC rail and sensor/guard branch during startup,
normal radio load, power sequencing, unplug/replug and controlled peripheral
overload/fault conditions. Use a current-limited bench supply/electronic load
and fault fixture appropriate to the protected branch; record the fault level,
duration, protective-device response and instrument setup. Include unpowered
module signal/backfeed behavior relevant to the actual harness.

**Pass:** FC rails stay within declared limits, the FC/control link behaves as
required, and module faults cannot cause the disallowed reset/flight-control
effects. Retain voltage/current traces and FC reset/event logs. If protection is
insufficient, change the power design and repeat affected integration cases.
Separate USB bench power, nominal regulator ratings or a UART filter do not
qualify the installed flight power system.

## PV-08 — Review and close the recorded installation

Complete this table in the run record. `Not run`, `failed` and justified
`not applicable` are distinct from `passed`.

| Case | Status at handoff | Result/evidence to fill later |
| --- | --- | --- |
| PV-01 candidate, limits and harness | Pending; guard identity/build already recorded | |
| PV-02 guard fixture | Not run | |
| PV-03 delivery/operator path | Not run on hardware | |
| PV-04 authority isolation | Partial simulator coverage only | |
| PV-05 sustained load/recovery | Brief simulator smoke tests only | |
| PV-06 failure/reset | Not run for installed UART/FC | |
| PV-07 electrical containment | Not run | |
| PV-08 release review | Pending all applicable results | |

The reviewer checks raw evidence against the predeclared limits, missing
preconditions, deviations and residual issues. Record operator/reviewer names,
date, exact topology and image/configuration hashes. Close TM-04's remaining
verification only after its applicable authority/UART/timing/failure cases pass;
close TM-11 only after PV-07 passes. Full P0 closure requires all applicable
cases, including actual report delivery, for the named installation. A justified
guard-only case exclusion on an FC-only route does not waive shared acceptance.

Update [the register](threat-register.md), [hardening plan](hardening-plan.md),
[acceptance checklist](p0-acceptance.md) and
[progress record](evidence/p0-closure-progress.json) with links to the new results.
Until then retain **partially closed — pending full validation**. Changed board,
FC version, policy, wiring, power design or signing/routing configuration requires
an impact review and repetition of affected cases; one accepted installation
does not qualify other variants. P1 authentication, provisioning and RF robustness
work remains separate.
