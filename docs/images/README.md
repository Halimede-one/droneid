# Documentation images

## Web bench screenshot

[`web-bench.png`](web-bench.png) is an unedited capture of the local web bench
with two USB devices connected and the Device settings tab selected, including
the UART/MAVLink panel. Captured during the firmware 0.5.0 bench session on
2026-10-23; device names, firmware versions, and counters reflect that session.
It is embedded in the [README web bench section](../../README.md#open-the-web-bench).

## Wiring illustration

[`micoair-xiao-uart4.png`](micoair-xiao-uart4.png) is the annotated wiring
illustration embedded in the project README. It was created on 2026-10-23 with
the built-in image-generation tool, using the five user-supplied Seeed/MicoAir
board photographs and pinout/wiring diagrams as references.

The FC view is the underside; the XIAO view is the front with USB at the top.
The UART4 enlargement is a logical signal breakout, not a numbered cable-pin
diagram. Use the [README's wiring table](../../README.md#micoair-h743-v2-aio-example--uart4)
and the manual for the actual board revision when making the harness.

The supplied MicoAir diagram is labelled H743V2-AIO-45A. The manufacturer's
current product page is for a 35A variant, so the illustration uses the V2 AIO
family name without a current rating. It does not establish physical harness
or end-to-end firmware compatibility; those remain bench checks.

Prompt set, in generation order:

1. [Initial composition](micoair-xiao-uart4.prompt.txt)
2. [Wire routing corrections](micoair-xiao-uart4.edit-prompt.txt)
3. [Lower pad and breakout corrections](micoair-xiao-uart4.final-prompt.txt)
4. [Final callout alignment](micoair-xiao-uart4.alignment-prompt.txt)

Reference documentation:

- [Seeed XIAO ESP32-C5 pin map](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/#pin-map)
- [MicoAir V2 AIO product, ports, and firmware](https://micoair.com/flightcontroller_micoair743v2_aio_35a/)
- [ArduCopter 4.6.3 MicoAir743v2 hardware definition](https://github.com/ArduPilot/ardupilot/blob/Copter-4.6.3/libraries/AP_HAL_ChibiOS/hwdef/MicoAir743v2/hwdef.dat)
