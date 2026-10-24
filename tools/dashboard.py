"""Local multi-device USB bench service. Only the Device reader owns a monitor port."""

from collections import deque
from datetime import datetime, timezone
import hashlib
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time

import bench

ROOT = Path(__file__).resolve().parents[1]
MAX_LOGS = 500
MAX_CONTACTS = 128


def now_utc():
    return datetime.now(timezone.utc).isoformat()


def discover():
    """Keys follow USB serial identity, never a reusable tty number."""
    result = {}
    for port in bench.boards():
        if not port.serial_number:
            continue  # Cannot safely track an unidentified port through reconnect.
        serial = str(port.serial_number)
        clean = serial.replace(':', '').lower()
        key = clean if re.fullmatch('[0-9a-f]{12}', clean) else hashlib.sha256(serial.encode()).hexdigest()[:20]
        path = port.device
        for link in Path('/dev/serial/by-id').glob('*'):
            if link.resolve() == Path(path).resolve():
                path = str(link)
                break
        result[key] = {'id': key, 'serial': serial, 'port': path, 'tty': port.device}
    return result


def identity_text(data):
    text = data.get('uas_id_text')
    if isinstance(text, str) and text:
        return text[:40]
    encoded = data.get('uas_id_hex')
    if not isinstance(encoded, str) or not re.fullmatch('[0-9a-fA-F]{40}', encoded):
        return None
    raw = bytes.fromhex(encoded)
    if data.get('id_type') == 4 and raw[0] == 255:
        text = raw[1:].rstrip(b'\0')
        if text and all(32 <= c <= 126 for c in text):
            return text.decode('ascii')
    return encoded


class Contacts:
    """Source-based aggregation; identity and hello keep separate receive ages."""
    def __init__(self):
        self.items = {}

    def ingest(self, record, timestamp):
        address = record.get('source_address')
        transport = record.get('transport')
        if not isinstance(address, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', address):
            return
        if transport not in ('ble_legacy', 'wifi_beacon', 'wifi_nan'):
            return
        key = ('wifi' if transport.startswith('wifi') else f'ble-{record.get("source_address_type")}') + ':' + address
        age = 0
        rx, reported = record.get('rx_us'), record.get('reported_us')
        ordered_rx = rx if type(rx) is int and 0 <= rx <= 2**64-1 else None
        if ordered_rx is not None and type(reported) is int and rx <= reported <= 2**64-1:
            age = min((reported - rx) / 1e6, 86400)
        received = timestamp - age
        if key not in self.items:
            if len(self.items) >= MAX_CONTACTS:
                del self.items[min(self.items, key=lambda k: self.items[k]['last_seen'])]
            self.items[key] = {'key': key, 'source_address': address, 'id': None, 'id_at': None,
                'hello': None, 'hello_at': None, 'last_seen': received, 'first_seen': received,
                'rssi_dbm': None, 'history': [], 'count': 0, 'transport': transport, 'channel': None,
                'last_rx_us': ordered_rx}
        item = self.items[key]
        item['count'] += 1
        # A Wi-Fi pack shares rx_us across messages, but formatting/USB timing
        # varies. Order by the radio timestamp, not the estimated host time.
        if ordered_rx is not None and item['last_rx_us'] is not None and ordered_rx < item['last_rx_us']:
            return
        item.update(last_seen=max(received,item['last_seen']), transport=transport,
                    channel=record.get('channel'), last_rx_us=ordered_rx)
        rssi = record.get('rssi_dbm')
        if type(rssi) is int and -127 <= rssi <= 126:
            item['rssi_dbm'] = rssi
            item['history'] = (item['history'] + [rssi])[-40:]
        data = record.get('data')
        if record.get('decode_status') != 'ok' or not isinstance(data, dict):
            return
        if record.get('message_type') == 'basic_id':
            name = identity_text(data)
            if name:
                if item['id'] is not None and item['id'] != name:
                    item['hello'] = item['hello_at'] = None
                item['id'], item['id_at'] = name, received
        elif record.get('message_type') == 'self_id' and isinstance(data.get('description'), str):
            item['hello'], item['hello_at'] = data['description'][:23], received

    def snapshot(self, timestamp):
        self.items = {k: v for k, v in self.items.items() if timestamp - v['last_seen'] < 120}
        return [dict(v, age_s=max(0, timestamp - v['last_seen']),
                     id_age_s=None if v['id_at'] is None else max(0, timestamp - v['id_at']),
                     hello_age_s=None if v['hello_at'] is None else max(0, timestamp - v['hello_at']))
                for v in sorted(self.items.values(), key=lambda x: x['last_seen'], reverse=True)]


class Firmware:
    def __init__(self, build_dir=ROOT / 'build'):
        self.build_dir = Path(build_dir).resolve()

    def bundle(self):
        root = self.build_dir
        manifest = json.loads((root / 'flasher_args.json').read_text())
        description = json.loads((root / 'project_description.json').read_text())
        if manifest.get('extra_esptool_args', {}).get('chip') != 'esp32c5' or description.get('target') != 'esp32c5':
            raise ValueError('Build must target ESP32-C5.')
        if description.get('project_name') != 'droneid':
            raise ValueError('Build is not the DroneID project.')
        files = manifest.get('flash_files', {})
        if set(files) != {'0x2000', '0x8000', '0x10000'}:
            raise ValueError('Unexpected flash layout. Build the project with its supplied partition table.')
        digest = hashlib.sha256()
        images = []
        for offset, name in sorted(files.items(), key=lambda pair: int(pair[0], 16)):
            path = (root / name).resolve()
            if not path.is_relative_to(root):
                raise ValueError('Firmware image is outside the build directory.')
            data = path.read_bytes()
            limit = {'0x2000': 0x6000, '0x8000': 0x1000, '0x10000': 0x200000}[offset]
            if not data or len(data) > limit:
                raise ValueError('Firmware image does not fit its partition.')
            digest.update(offset.encode() + b'\0' + data)
            images.append((offset, path.name, data))
        return {'version': description['project_version'], 'chip': 'esp32c5',
                'fingerprint': digest.hexdigest(), 'size': sum(len(i[2]) for i in images), 'images': images}

    def info(self):
        try:
            return {k: v for k, v in self.bundle().items() if k != 'images'} | {'available': True}
        except (OSError, ValueError, KeyError, TypeError) as exc:
            return {'available': False, 'error': f'Firmware unavailable: {exc}. Run idf.py build.'}


class Device:
    def __init__(self, info, locator=discover, log_dir=ROOT / 'logs' / 'web', start=True):
        self.info = info
        self.locator = locator
        self.lock = threading.RLock()
        self.operation_lock = threading.Lock()
        self.port_lock = threading.Lock()
        self.stop = threading.Event()
        self.pause = threading.Event()
        self.closed = threading.Event()
        self.closed.set()
        self.monitoring = True
        self.present = True
        self.connection = 'connecting'
        self.error = None
        self.status = None
        self.status_at = None
        self.session = None
        self.contacts = Contacts()
        self.logs = deque(maxlen=MAX_LOGS)
        self.log_cursor = 0
        self.job = None
        self.last_seen_usb = time.time()
        Path(log_dir).mkdir(parents=True, exist_ok=True)
        self.log_handler = RotatingFileHandler(Path(log_dir) / (info['id'] + '.jsonl'), maxBytes=5*1024*1024, backupCount=2, encoding='utf-8')
        self.log_handler.setFormatter(logging.Formatter('%(message)s'))
        self.thread = threading.Thread(target=self.reader, daemon=True, name='usb-' + info['id'])
        if start:
            self.thread.start()

    def fresh_port(self):
        info = self.locator().get(self.info['id'])
        if not info or info['serial'] != self.info['serial']:
            raise OSError('Selected board is not connected. Reconnect that board and retry.')
        return info['port']

    def record(self, record):
        timestamp = time.time()
        record = dict(record, host_received_utc=now_utc())
        if record.get('kind') == 'status' and re.fullmatch('[0-9a-f]{12}', self.info['id']):
            if record.get('receiver_id') != self.info['id']:
                raise OSError('USB identity does not match the responding firmware. Monitoring stopped.')
        with self.lock:
            session = record.get('session')
            if isinstance(session, str) and session != self.session:
                self.contacts = Contacts()
                self.status = None
                self.session = session
            if record['kind'] == 'status':
                self.status = record
                self.status_at = timestamp
                self.error = None
            elif record['kind'] == 'observation':
                self.contacts.ingest(record, timestamp)
            self.log_cursor += 1
            self.logs.append({'cursor': self.log_cursor, 'record': record})
        try:
            self.log_handler.emit(logging.LogRecord('usb', logging.INFO, '', 0, json.dumps(record, allow_nan=False), (), None))
        except OSError as exc:
            with self.lock:
                self.error = f'Log file could not be written: {exc}'

    def reader(self):
        serial, _ = bench.serial_modules()
        while not self.stop.is_set():
            if self.pause.is_set() or not self.monitoring or not self.present:
                self.closed.set()
                self.stop.wait(.2)
                continue
            if not self.port_lock.acquire(timeout=.2):
                continue
            if self.pause.is_set() or not self.monitoring or not self.present:
                self.port_lock.release()
                continue
            self.closed.clear()
            decoder = bench.LineDecoder()
            try:
                with bench.connect(self.fresh_port()) as stream:
                    with self.lock:
                        self.connection = 'monitoring'
                        self.error = None
                    stream.write(b'\nstatus\n')
                    while not self.stop.is_set() and not self.pause.is_set() and self.monitoring and self.present:
                        for record in decoder.feed(stream.read(2048)):
                            self.record(record)
            except (serial.SerialException, OSError) as exc:
                with self.lock:
                    self.connection = 'waiting'
                    self.error = str(exc)
            finally:
                self.closed.set()
                self.port_lock.release()
                with self.lock:
                    self.connection = 'paused' if self.pause.is_set() or not self.monitoring else 'waiting'
            self.stop.wait(.5)
        self.log_handler.close()

    def snapshot(self):
        with self.lock:
            now = time.time()
            return dict(self.info, present=self.present, monitoring=self.monitoring,
                        connection=self.connection, error=self.error, status=self.status,
                        status_age_s=None if self.status_at is None else now-self.status_at,
                        contacts=self.contacts.snapshot(now), log_cursor=self.log_cursor,
                        job=None if self.job is None else dict(self.job, lines=list(self.job['lines'])))

    def job_line(self, text):
        text = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', text).strip()[:400]
        if text:
            with self.lock:
                self.job['lines'].append(text)

    def begin(self, kind, work):
        if not self.operation_lock.acquire(blocking=False):
            raise RuntimeError('This board already has an operation in progress.')
        with self.lock:
            self.job = {'id': secrets.token_hex(8), 'kind': kind, 'state': 'running',
                        'started_at': now_utc(), 'lines': deque(maxlen=100), 'error': None}
            job_id = self.job['id']

        def run():
            previous_monitoring = self.monitoring
            try:
                self.pause.set()
                if not self.port_lock.acquire(timeout=3):
                    raise RuntimeError('USB monitor did not release the port.')
                self.job_line('USB monitor paused for this board.')
                try:
                    verify = work()
                finally:
                    self.port_lock.release()
                # Reopen after the firmware's acknowledged restart, then verify
                # a fresh status. Never show success solely from an NVS ACK.
                with self.lock:
                    self.status = None
                    self.status_at = None
                self.monitoring = True
                self.pause.clear()
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    with self.lock:
                        status = self.status
                    if status and status.get('radio_ready') and verify(status):
                        self.job_line('Verified settings and running firmware over USB.')
                        with self.lock:
                            self.job['state'] = 'complete'
                        break
                    self.stop.wait(.2)
                else:
                    raise RuntimeError('Operation finished, but the running settings could not be verified. Check USB and tap RESET, then refresh.')
            except Exception as exc:
                self.job_line(str(exc))
                with self.lock:
                    self.job['state'] = 'failed'
                    self.job['error'] = str(exc)
            finally:
                self.monitoring = previous_monitoring
                self.pause.clear()
                self.operation_lock.release()
        threading.Thread(target=run, daemon=True, name=kind+'-'+self.info['id']).start()
        return job_id

    def configure(self, values):
        command = bench.configuration_command(values.get('role'), values.get('transport'), values.get('broadcast_id'), values.get('hello'))
        with self.lock:
            if not self.status or self.status.get('configuration_version') != 1:
                raise ValueError('Flash firmware 0.4.0 or newer before setting an ID and hello message.')

        def work():
            self.job_line('Saving ID, hello message, role, and broadcast transport together…')
            bench.request(self.fresh_port(), command, timeout=10)
            self.job_line('Settings saved. Waiting for restart…')
            return lambda s: (s.get('role') == values['role'] and
                s.get('tx_transport') == {'wifi':'wifi_beacon','ble':'ble_legacy'}[values['transport']] and
                s.get('broadcast_id') == values['broadcast_id'] and
                s.get('hello_hex') == values['hello'].encode().hex())
        return self.begin('configure', work)

    def flash(self, firmware, expected_fingerprint):
        bundle = firmware.bundle()
        if expected_fingerprint != bundle['fingerprint']:
            raise ValueError('Firmware build changed. Refresh the page before flashing.')

        def work():
            port = self.fresh_port()
            with tempfile.TemporaryDirectory(prefix='droneid-flash-') as tmp:
                command = [sys.executable, '-m', 'esptool', '--chip', 'esp32c5', '--port', port,
                           '--baud', '460800', '--before', 'default-reset', '--after', 'hard-reset',
                           'write-flash', '--flash-mode', 'dio', '--flash-size', '8MB', '--flash-freq', '80m']
                for offset, name, data in bundle['images']:
                    path = Path(tmp) / (offset + '-' + name)
                    path.write_bytes(data)
                    command.extend([offset, str(path)])
                self.job_line(f'Flashing DroneID {bundle["version"]} to {self.info["serial"]}.')
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1, env=dict(os.environ, PYTHONUNBUFFERED='1', NO_COLOR='1', TERM='dumb'))
                timer = threading.Timer(120, process.kill)
                timer.daemon = True
                timer.start()
                try:
                    for line in process.stdout:
                        self.job_line(line)
                    if process.wait() != 0:
                        raise RuntimeError('Flashing failed. See output below. If needed: hold BOOT, tap RESET, release BOOT, then retry.')
                finally:
                    timer.cancel()
                    process.stdout.close()
            self.job_line('Flash verified by esptool. Waiting for firmware…')
            return lambda s: s.get('firmware') == bundle['version']
        return self.begin('flash', work)

    def configure_uart(self, values):
        command=bench.uart_command(values)
        with self.lock:
            if not self.status or (self.status.get('uart') or {}).get('configuration_version')!=1:
                raise ValueError('Flash firmware 0.5.0 or newer before configuring UART.')
        def work():
            self.job_line('Saving flight-controller UART settings…')
            bench.request(self.fresh_port(),command,timeout=10)
            return lambda status: all((status.get('uart') or {}).get(k)==v for k,v in values.items()) and (
                not values['enabled'] or (status.get('uart') or {}).get('ready') is True)
        return self.begin('uart',work)

    def set_monitoring(self, enabled):
        if not self.operation_lock.acquire(blocking=False):
            raise RuntimeError('Wait for this board’s operation to finish.')
        try:
            self.monitoring = enabled
            if not enabled:
                self.closed.wait(3)
        finally:
            self.operation_lock.release()


class Manager:
    def __init__(self, firmware=None, locator=discover):
        self.devices = {}
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.firmware = firmware or Firmware()
        self.locator = locator
        self.discovery_error = None
        self.token = secrets.token_urlsafe(32)
        self.thread = threading.Thread(target=self.discovery_loop, daemon=True)

    def discovery_loop(self):
        while not self.stop.is_set():
            try:
                found = self.locator()
                with self.lock:
                    for device in self.devices.values():
                        device.present = device.info['id'] in found
                    for key, info in found.items():
                        if key not in self.devices:
                            if len(self.devices) >= 32:
                                continue
                            self.devices[key] = Device(info, locator=self.locator)
                        else:
                            with self.devices[key].lock:
                                self.devices[key].info = info
                                self.devices[key].last_seen_usb = time.time()
                    self.discovery_error = None
            except Exception as exc:
                self.discovery_error = str(exc)
            self.stop.wait(1)

    def snapshot(self):
        with self.lock:
            return {'devices': [d.snapshot() for d in self.devices.values()], 'server_time': time.time(),
                    'discovery_error': self.discovery_error}

    def device(self, key):
        with self.lock:
            if key not in self.devices:
                raise KeyError('Unknown device.')
            return self.devices[key]

    def close(self):
        self.stop.set()
        with self.lock:
            devices = list(self.devices.values())
        for device in devices:
            if device.operation_lock.acquire(timeout=140):
                device.operation_lock.release()
            device.stop.set()
        for device in devices:
            if device.thread.is_alive():
                device.thread.join(3)
