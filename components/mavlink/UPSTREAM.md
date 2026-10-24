# MAVLink C library

Unmodified headers from https://github.com/mavlink/c_library_v2 at commit
`6a7760569ce6d46c56f93163703543d4345e940a`, retrieved 2026-10-23.
Included: root support headers and common/standard/minimal generated C headers,
excluding their test suites. No custom MAVLink message IDs are allocated.

`COPYING` is the upstream MAVLink licensing notice, including the MIT exception
for generated code, from https://github.com/mavlink/mavlink/blob/master/COPYING.
The application uses standard HEARTBEAT and TUNNEL messages and reads a limited
set of common telemetry messages. The project-specific TUNNEL payload is
documented in docs/uart-protocol.md.
