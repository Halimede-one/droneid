#!/usr/bin/env python3
"""Read DroneID USB JSON records or persist a board's bench role."""

import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import re
from pathlib import Path
import sys
import time
import uuid


def reject_constant(value):
    raise ValueError(f"Non-JSON numeric constant: {value}")


class LineDecoder:
    """Bound memory and recover at a newline after noise or a partial record."""

    def __init__(self, limit=4096):
        self.limit = limit
        self.buffer = bytearray()
        self.discarding = False
        self.invalid_lines = 0

    def feed(self, chunk):
        records = []
        for byte in chunk:
            if byte == 10:
                if not self.discarding and self.buffer:
                    try:
                        record = json.loads(self.buffer, parse_constant=reject_constant)
                        if (not isinstance(record, dict) or
                                type(record.get("schema")) is not int or record['schema'] != 1):
                            raise ValueError("Unsupported record schema")
                        if record.get("kind") not in ("observation", "status", "reply"):
                            raise ValueError("Unknown record kind")
                        if record["kind"] == "observation":
                            rssi = record.get("rssi_dbm")
                            if rssi is not None and (type(rssi) is not int or not -127 <= rssi <= 126):
                                raise ValueError("Invalid RSSI")
                        records.append(record)
                    except (ValueError, UnicodeError, RecursionError):
                        self.invalid_lines += 1
                self.buffer.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.buffer) >= self.limit:
                    self.buffer.clear()
                    self.discarding = True
                    self.invalid_lines += 1
                else:
                    self.buffer.append(byte)
        return records


def serial_modules():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        raise SystemExit("Install the host dependency: python -m pip install -r tools/requirements.txt")
    return serial, list_ports


def boards():
    _, list_ports = serial_modules()
    return sorted((p for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001),
                  key=lambda p: p.device)


def select_port(explicit):
    if explicit:
        return explicit
    found = boards()
    if len(found) != 1:
        raise SystemExit(f"Found {len(found)} Espressif boards. Use 'list', then specify --port.")
    # Prefer a persistent path, so reconnect cannot silently switch boards.
    for link in sorted(Path('/dev/serial/by-id').glob('*')):
        if link.resolve() == Path(found[0].device).resolve():
            return str(link)
    return found[0].device


def connect(port):
    serial, _ = serial_modules()
    device = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=1)
    device.dtr = False
    device.rts = False
    if sys.platform != 'win32':
        device.exclusive = True
    device.port = port
    device.open()
    return device


def human_record(record):
    if record['kind'] == 'observation':
        rssi = record.get('rssi_dbm')
        strength = 'unknown RSSI' if rssi is None else f'{rssi} dBm'
        data = record.get('data') or {}
        channel = record.get('channel')
        radio = record.get('transport', '?') + (f'/ch{channel}' if channel else '')
        return (f"{record.get('receiver_id')} <- {record.get('source_address')} "
                f"{strength:>12} {radio}  {record.get('message_type')} "
                f"seq={record.get('seq')}  {json.dumps(data, ensure_ascii=True)}")
    if record['kind'] == 'status':
        return (f"[{record.get('receiver_id')}] role={record.get('role')} "
                f"tx={record.get('tx_transport', '?')} phase={record.get('rx_phase', '?')} "
                f"ready={record.get('radio_ready')} received={record.get('rid_observations')} "
                f"queue_drops={record.get('rx_queue_dropped')} "
                f"usb_drops={record.get('usb_records_dropped')} "
                f"radio_errors={record.get('radio_errors')}")
    return json.dumps(record, ensure_ascii=True)


def request(port, operation, timeout=8):
    serial, _ = serial_modules()
    token = uuid.uuid4().hex[:12]
    deadline = time.monotonic() + timeout
    # Status and saved-setting assignments are idempotent. A previous command
    # can still be rebooting/re-enumerating USB when the next one opens the port.
    while time.monotonic() < deadline:
        decoder = LineDecoder()
        try:
            with connect(port) as device:
                device.write(f'\n{operation} {token}\n'.encode('ascii'))
                while time.monotonic() < deadline:
                    for record in decoder.feed(device.read(1024)):
                        if record.get('request_id') == token:
                            if record['kind'] == 'reply' and not record.get('ok'):
                                raise RuntimeError(record.get('message', 'Command failed'))
                            return record
        except (serial.SerialException, OSError):
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))
    raise TimeoutError('No matching reply. Check firmware and close other serial monitors.')


def configuration_command(role, transport, broadcast_id, hello):
    if role not in ('rx', 'tx', 'both') or transport not in ('wifi', 'ble'):
        raise ValueError('Choose a valid role and one broadcast transport.')
    if not isinstance(broadcast_id, str) or not re.fullmatch(r'[A-Za-z0-9._-]{1,19}', broadcast_id):
        raise ValueError('ID must be 1–19 letters, numbers, dots, underscores, or hyphens.')
    if not isinstance(hello, str) or len(hello) > 23 or any(not 32 <= ord(c) <= 126 for c in hello):
        raise ValueError('Hello message must contain at most 23 printable ASCII characters.')
    return f'configure {role} {transport} {broadcast_id.encode().hex()} {hello.encode().hex() or "-"}'


UART_DEFAULTS = dict(enabled=False, baud=115200, tx_pin=11, rx_pin=12,
                     system_id=1, component_id=242, fc_component_id=1,
                     target_system=255, target_component=190, max_rate=5)


def uart_command(values):
    if set(values) != set(UART_DEFAULTS):
        raise ValueError('Supply all UART settings.')
    if type(values['enabled']) is not bool or any(type(values[k]) is not int for k in values if k!='enabled'):
        raise ValueError('UART fields must be integers and enabled must be boolean.')
    if values['baud'] not in (57600,115200,230400,460800):
        raise ValueError('Choose a supported UART baud rate.')
    pins={1,0,25,7,23,24,11,12,8,9,10}
    if values['tx_pin'] not in pins or values['rx_pin'] not in pins or values['tx_pin']==values['rx_pin']:
        raise ValueError('Choose two different XIAO header GPIO pins.')
    if any(not 1<=values[k]<=255 for k in ('system_id','component_id','fc_component_id')):
        raise ValueError('MAVLink source IDs must be 1–255.')
    if values['component_id']==values['fc_component_id']:
        raise ValueError('Module and flight controller component IDs must differ.')
    if any(not 0<=values[k]<=255 for k in ('target_system','target_component')):
        raise ValueError('Destination IDs must be 0–255.')
    if (values['target_system'],values['target_component'])==(values['system_id'],values['component_id']):
        raise ValueError('The module cannot address reports to itself.')
    if not 1<=values['max_rate']<=50:
        raise ValueError('Report rate must be 1–50 observations/second.')
    return 'uart '+' '.join(str(int(values[k])) for k in UART_DEFAULTS)


def monitor(args, port):
    serial, _ = serial_modules()
    deadline = time.monotonic() + args.duration if args.duration else float('inf')
    destination = open(args.output, 'a', encoding='utf-8') if args.output else nullcontext(None)
    with destination as logfile:
        while time.monotonic() < deadline:
            decoder = LineDecoder()
            try:
                with connect(port) as device:
                    print(f'Connected to {port}', file=sys.stderr)
                    device.write(b'\nstatus\n')
                    while time.monotonic() < deadline:
                        for record in decoder.feed(device.read(2048)):
                            record['host_received_utc'] = datetime.now(timezone.utc).isoformat()
                            encoded = json.dumps(record, ensure_ascii=True, allow_nan=False)
                            if logfile:
                                logfile.write(encoded + '\n')
                                logfile.flush()
                            print(encoded if args.json else human_record(record), flush=True)
            except (serial.SerialException, OSError) as exc:
                print(f'USB unavailable: {exc}; retrying the same port', file=sys.stderr)
                time.sleep(min(0.5, max(0, deadline - time.monotonic())))
            finally:
                if decoder.invalid_lines:
                    print(f'Ignored {decoder.invalid_lines} incomplete/non-record lines', file=sys.stderr)


def positive_seconds(value):
    parsed = float(value)
    if not 0 < parsed < float('inf'):
        raise argparse.ArgumentTypeError('duration must be finite and greater than zero')
    return parsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list', help='List Espressif USB Serial/JTAG boards')
    for name in ('status', 'role', 'transport', 'configure', 'uart', 'monitor'):
        sub = commands.add_parser(name)
        sub.add_argument('--port', help='Device path; auto-select only if exactly one board is present')
        if name == 'role':
            sub.add_argument('role', choices=('rx', 'tx', 'both'), help='Saved to flash; restarts the board')
        if name == 'transport':
            sub.add_argument('transport', choices=('wifi', 'ble'),
                             help='Broadcast transport only; saved to flash; restarts the board')
        if name == 'configure':
            sub.add_argument('--role', required=True, choices=('rx', 'tx', 'both'))
            sub.add_argument('--transport', default='wifi', choices=('wifi', 'ble'))
            sub.add_argument('--id', required=True, dest='broadcast_id')
            sub.add_argument('--hello', default='')
        if name == 'uart':
            enable=sub.add_mutually_exclusive_group(required=True)
            enable.add_argument('--enable',dest='enabled',action='store_true')
            enable.add_argument('--disable',dest='enabled',action='store_false')
            for key,value in UART_DEFAULTS.items():
                if key!='enabled':sub.add_argument('--'+key.replace('_','-'),type=int,default=value)
        if name == 'monitor':
            sub.add_argument('--output', help='Append all records with host UTC timestamps to JSONL')
            sub.add_argument('--json', action='store_true', help='Print JSONL instead of human-readable lines')
            sub.add_argument('--duration', type=positive_seconds, help='Stop after this many seconds')
    args = parser.parse_args()
    serial, _ = serial_modules()
    try:
        if args.command == 'list':
            found = boards()
            for board in found:
                print(f'{board.device}\t{board.serial_number}\t{board.description}')
            if not found:
                print('No Espressif USB Serial/JTAG boards found.', file=sys.stderr)
            return
        port = select_port(args.port)
        if args.command == 'monitor':
            monitor(args, port)
        else:
            operation = (f'role {args.role}' if args.command == 'role' else
                         f'transport {args.transport}' if args.command == 'transport' else
                         configuration_command(args.role, args.transport, args.broadcast_id, args.hello)
                         if args.command == 'configure' else
                         uart_command({k:getattr(args,k) for k in UART_DEFAULTS}) if args.command=='uart' else 'status')
            print(json.dumps(request(port, operation), indent=2))
    except KeyboardInterrupt:
        pass
    except (serial.SerialException, OSError, RuntimeError, ValueError) as exc:
        raise SystemExit(str(exc))


if __name__ == '__main__':
    main()
