# Local web bench

The dashboard programs and monitors multiple XIAO ESP32-C5 boards over USB.
It runs on the computer attached to the boards. No cloud service, Node.js,
browser USB permission, or frontend build is needed.

## Start

Open this project in an ESP-IDF terminal in VS Code:

```sh
python -m pip install -r tools/requirements.txt
idf.py build
python tools/server.py
```

Open **http://127.0.0.1:8766**. The server binds only to the local computer.
If the port is occupied, use `python tools/server.py --port 8767` and open the
printed address. Stop the server with Ctrl+C.

Close VS Code's serial monitor and any `bench.py monitor` processes first.
Only one reader can own a USB port. Opening a serial port resets these boards
on the tested Linux host; saved settings survive, and the UI starts a new
observation session.

## Program and configure a board

1. Plug in a board with a USB data cable. It appears in the sidebar automatically,
   tracked by its USB serial number, even when its tty number changes.
2. Select it and open **Device settings**. Check **Target USB serial**, then
   click **Flash this device**. This writes the local project's bootloader,
   partition table, and application, and verifies the running firmware over USB.
3. Set its **Broadcast ID** and **Hello message**. Choose **Receive + broadcast**
   to have it hear other boards while sending its own identity. Select Wi-Fi
   Beacon (the default) or Bluetooth LE as its one broadcast transport.
4. Click **Save to device**. The app saves all settings together, waits for the
   restart, and verifies them. Repeat for each board.

The broadcast label supports 1–19 ASCII letters, digits, dots, underscores, and
hyphens. The hello message supports up to 23 printable ASCII characters and may
be empty. Settings persist on the board, so a configured transmitter can run
from a USB charger or power bank.

A fresh board defaults to receive-only. Upgrades preserve saved roles and
transports; firmware 0.4.0 adds persistent identity/message settings. Flashing
the supplied partition layout preserves NVS settings. Build with `idf.py build`
after firmware edits, then **Refresh devices** to load the new build fingerprint.
The dashboard flashes an existing local build; it does not run the compiler.

If flashing fails, expand **Operation output**. For ROM bootloader recovery,
hold BOOT, tap RESET, release BOOT, and retry **Flash this device**. A new board
without this firmware can still be flashed even though no running version or
configuration is reported yet.

## Watch the devices

**Nearby broadcasts** is the default view. It draws the selected receiver and
observed sources, with their claimed IDs, hello messages, RSSI in dBm, and recent
signal history. It is a schematic: signal strength does not establish distance
or bearing. A source is marked active for five seconds after its latest message;
older sources fade and disappear after two minutes. ID and hello ages are
tracked independently. Hover over the message to see its age.

Sources are grouped by radio address and transport family, not by claimed ID.
Two sources claiming the same ID stay separate. A device observed on BLE and
Wi-Fi can appear twice. Radio claims are displayed as received, without treating
an ID as authenticated fleet membership.

**Live log** tails the selected board's JSONL records. Filter observations,
status, or replies, pause scrolling, and download the most recent 500 records.
The USB protocol is schema-1 JSONL, serving as the bench substitute for the
planned UART/MAVLink integration.

Each connected board has its own reader, log, contacts, and operation status.
**Pause monitor** releases only that board's serial port for another tool;
it does not disable the board's radio. **Start monitor** resumes collection.
Flashing/configuring a selected board pauses and restores its reader, while
other boards continue monitoring. A board powered without a data connection
continues its saved radio mode, but cannot provide its receive log to this app.

Combined mode interleaves TX/RX and BLE/Wi-Fi receive windows. Reception is not
continuous on every channel, and messages can be missed outside matching windows.
Coverage and timing are documented in the [USB bench guide](usb-bench.md).

## Logs and scope

The server keeps the last 500 records per board in memory and writes captures
to `logs/web/<usb-id>.jsonl`, rotating at 5 MiB with two backups. It tracks at most
128 observed sources per board and 32 USB board identities per server run.
Logs are local and git-ignored. There is no replay of data missed while USB
monitoring was stopped; firmware drop counters remain visible in the dashboard.

Firmware 0.5.0 adds **Device settings → UART · MAVLink 2**, including an enable
switch, baud, GPIO pins, source/destination IDs, report-rate limit, and live UART
counters. Its wiring link opens the project README. UART settings are saved
independently from radio settings and default to disabled.

This build sends experimental Session IDs and public Self ID descriptions for
bench testing. It does not yet broadcast own-aircraft position. Hardware UART now reports
observations using MAVLink 2 while USB continues to carry JSONL. See the [project definition](project-definition.md)
for the vehicle integration scope.

## Validation

`bash tests/run_host_tests.sh` runs portable C tests with address/undefined-behavior
sanitizers and 15 Python tests. Coverage includes message framing, identity
validation, persistent-command construction, stale fields, duplicate claimed
IDs, Wi-Fi pack aggregation, bounded logs, firmware manifest restrictions,
independent serial-port ownership, and HTTP request guards.

Firmware 0.4.0 was built with ESP-IDF v6.1. Both attached boards were flashed
and configured through the browser as `SCOUT-01` and `SCOUT-02`, each with its own
hello message, in combined mode with Wi-Fi transmission. Both received the
other board's identity, hello, and RSSI. Browser checks cover device switching,
live logs, downloading records, unsaved form retention, per-board pause/resume,
desktop/mobile layouts, and treating received markup as plain text. Reflashing
each configured board preserved its settings and left the other board's boot
session unchanged.

A longer check exposed long gaps with a fixed Wi-Fi channel order. Firmware
0.4.0 now randomizes the two Wi-Fi windows each cycle. In the subsequent
60-second combined-mode run, the boards reported 88 and 124 received message
records at the last sample, with zero radio errors. The largest sampled source
ages were 5.11 and 5.99 seconds. These are sampled bench results, not a range or
maximum update-gap guarantee.
