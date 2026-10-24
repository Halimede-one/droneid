# Hardening plan

This is a proposed implementation backlog derived from the
[threat register](threat-register.md). The review added documentation and offline
checks; it did not change firmware behavior, flash devices, configure a flight
controller or burn eFuses. Existing unrelated working-tree changes are recorded
in the [baseline](evidence/baseline.json).

## P0 — Contain the flight-controller boundary

**Security requirement:** arbitrary bytes from a fully compromised ESP32 must
not authorize flight commands, change missions/settings, impersonate a trusted
controller, or exhaust the flight controller's communication/processing budget.
Test this independently of the ESP32's own good behavior.

Choose and validate one of these integration designs:

| Design | Effect | Tradeoff |
| --- | --- | --- |
| Physically omit ESP TX → FC RX; retain FC TX → ESP RX | Removes the module's direct serial injection path into the FC. Suitable for initial iNav telemetry-input testing. | Observation reports need a separate USB/downlink/gateway path. Shared power remains a separate concern. |
| Independent guard between ESP TX and FC RX | A separate, minimal processor filters and budgets all module traffic before it reaches the FC. | Adds hardware/firmware that must itself be reviewed and fail safely. |
| Enforced FC ingress policy on that physical port | Restricts the module inside the FC's trust domain. | Requires verified support or a controller firmware change; source-ID filtering alone is insufficient, and rejected floods still cost FC work. |

No stock setting in this project has been shown to implement the last two
designs. The README's MAVLink setup establishes connectivity, not a security
boundary. The iNav example is not read-only merely because its UI says
“Telemetry Output.”

A proposed guard policy should:

1. Parse and validate complete frames, lengths, CRC, supported flags and any
   signature policy before releasing output. Handle truncation, concatenation,
   noise and resynchronization without forwarding partial input.
2. Permit only the required component heartbeat and exact DroneID TUNNEL schema.
   Validate `payload_type=0`, `DRID`, version 1, kinds 1/2, lengths 78/84,
   field ranges and receive/report ordering. Regenerate trusted envelope fields
   and a fixed non-autopilot heartbeat rather than trusting claimed source IDs.
3. Fix allowed destinations to the chosen base station. Reject other message
   types, destinations and general passthrough. Do not interpret embedded RID
   bytes as MAVLink or other commands. Restrict routing in both directions to
   the messages actually required by the integration.
4. Enforce total byte/frame/work budgets independently of the ESP32, with
   bounded storage, observation rate limits, liveness counters and predictable
   overload behavior. A flood of rejected messages must also be bounded.
5. Keep module authentication separate from authority to control the vehicle.
   Do not solve an unsigned sensor interface by handing it a key that grants
   unrestricted FC command authority. Review signing and key boundaries with
   the selected FC/downlink design.

**Acceptance evidence:** record exact controller target/version and exported
configuration. In simulation and then an isolated bench, feed the guard arbitrary
streams and non-permitted message classes, forged IDs, invalid CRC/lengths,
replays and saturation. Demonstrate that only the normalized permitted reports
leave the guard, FC mission/parameters/mode remain unchanged, and FC/control-link
latency stays within the vehicle's declared budget. Test resets, stuck input,
guard failure and loss of power. No claim of flight containment until those
results exist for the installed design.

At the same integration gate, measure shared-rail current and fault behavior
(TM-11). A guard must not create an alternate electrical failure path.

## P1 — Fix host input handling and expose uncertainty

| Change | Acceptance criterion | Threats |
| --- | --- | --- |
| Validate USB record schemas and all numeric values before committing state | Overflowing numbers, invalid types, oversized/nested data and malformed records are rejected; a valid following record works; other boards remain usable. Convert the TM-07 probe into a rejection/recovery regression. | TM-07 |
| Isolate reader failures and invalid state | Every reader failure produces an explicit per-device error; `/api/state` and other devices remain serializable and responsive. | TM-07, TM-09 |
| Label aircraft claims and receiver health | Identity/hello views visibly say unverified; fresh receipt does not imply fresh/authentic aircraft data; stale/overloaded/disconnected receivers are distinct from “no contacts.” | TM-01, TM-02 |
| Add global ingress budgets plus bounded fairness | Sustained valid floods cannot cause unbounded callback work, heap growth or an indefinite failure to resume. MAC rotation is included in tests. Record CPU/heap/drop/latency thresholds before testing. | TM-02, TM-03 |
| Preserve evidence semantics | Raw messages, receiver/session/sequence, local receive/report times, losses and claimed source remain available; reporter authentication and aircraft authentication are separate fields. | TM-01, TM-06 |
| Bound HTTP resources | Connection/read timeouts, concurrent requests and streaming clients have explicit limits; existing Host/Origin/token/CSP checks keep passing. | TM-09 |

Do not turn a replay cache or a MAC allowlist into a “friendly” decision. Those
can suppress duplicate work, but an untrusted sender can change counters and
addresses. Operators need to see both claimed identity and its trust level.

## P1 — Authenticate selected telemetry paths

Document the downlink topology first: which participants can transmit, where
routes are learned, which link is signed/authenticated, and which component is
trusted to make each claim. The stock helper's magic bytes and destination
checks are format checks, not credentials.

- Implement an authenticated reporter-to-base-station path and a replay policy
  covering receiver identity, reboot/session transitions, ordering and duplicate
  reports. Verify unauthorized sources and replays are rejected without dropping
  a legitimate newly provisioned session.
- For controller telemetry into the module, define which source is authoritative
  and verify it rather than relying on system/component IDs. Keep position
  diagnostic until that policy and its failure behavior are tested.
- Keep aircraft-identity trust separate. Authenticated delivery of an observation
  means a trusted reporter sent it; it does not mean the observed aircraft's
  broadcast was truthful.

MAVLink signing supplies message authentication and replay-related checks, not
confidentiality or a sensor permission model. Its use must be matched to the
FC's actual enforcement and key-sharing policy.
[MAVLink signing specification](https://mavlink.io/en/guide/message_signing.html).

## P1 — Establish a deployment build and maintenance policy

Maintain a recoverable development profile and a separately reviewed deployment
profile. For deployment, evaluate compiler stack protection, authenticated boot,
flash/NVS protection where secrets are stored, debug/download restrictions,
signed release provenance and a supported update/recovery procedure. Record
per-device eFuse/provisioning evidence instead of inferring protection from
`sdkconfig` or a reported version string.

Use spare boards to verify wrong-signer rejection, authorized recovery, key
backup/rotation/revocation and the effect on USB monitoring before irreversible
provisioning. Keep release private keys off ordinary bench hosts. Secure boot
does not remove runtime parser exposure or make a captured sensor an FC-trusted
authority. The platform mechanisms are described in the
[ESP32-C5 security overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/security/security.html).

Define who may use the bench workstation, access its serial ports, replace
firmware bundles and read logs. Loopback plus a CSRF token is a reasonable
single-user development assumption, not authentication against local malware
or another local account. Adding remote/multi-user access requires a separate
access-control design.

## Ongoing — Dependency and adversarial-input validation

Keep exact SDK, radio-library, codec, MAVLink, Python-tool and compiler versions
with releases. Triage upstream advisories for the actual target, feature and
reachable path; do not assume either “all ESP32s affected” or “no network service
means no radio risk.” Add coverage-guided fuzzing around the portable framing,
codec, serializer and MAVLink boundaries with retained regression inputs.

The [validation backlog](validation.md#remaining-validation) separates offline,
simulator and hardware work. Revisit this model whenever adding Wi-Fi association,
IP services, GATT/provisioning, OTA, live position broadcasts, fleet identity
verification, flight automation, FC forwarding or remote web-bench access.
