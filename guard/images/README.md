# Guard wiring illustration

[scout-guard-fc.png](scout-guard-fc.png) is the board-photo illustration in the
guard README. Created on 2026-10-24 with the built-in image-generation tool using
the [project wiring illustration](../../docs/images/micoair-xiao-uart4.png) as its
board-appearance/style reference. The original user-supplied board references
are described in the [project image notes](../../docs/images/README.md).

The exact generation prompt is [guard-wiring.prompt.txt](guard-wiring.prompt.txt).
The resulting Scout/Guard pad annotations and all four directed UART connections
were visually checked against the local firmware pin definitions and wiring table.
Both pictured XIAOs are **C5** boards, front-facing with USB at the top. The FC
is shown from underneath. The lower diagram is logical signal routing, not a
numbered connector or cable-end pinout. C6/S3 GPIO numbers differ.

The illustration uses separate USB bench power for the two XIAOs and the FC's
normal bench supply. It shows only data wires and common ground; VBUS/5V and
3V3 pads are unconnected. It does not establish electrical fault isolation or
hardware acceptance. Follow the [guard wiring instructions](../README.md#connection).

[wiring.svg](wiring.svg) remains available as the compact vector overview.
