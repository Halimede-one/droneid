#!/usr/bin/env python3
"""Run the pinned FC policy in a private, disarmed ArduCopter simulator.

Creates its own SITL process and loopback sockets; accepts no serial-device or
remote endpoint arguments. This is simulator evidence, not flight acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

from pymavlink.dialects.v20 import ardupilotmega as mav

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mavlink_receiver import decode_payload
from install import install


class Peer:
    def __init__(self, sock, system, component, capture):
        self.sock = sock
        self.sock.setblocking(False)
        self.mav = mav.MAVLink(None, srcSystem=system, srcComponent=component)
        self.parser = mav.MAVLink(None)
        self.parser.robust_parsing = True
        self.capture = capture
        self.received = []

    def write(self, data):
        self.capture.write(json.dumps({'time': time.monotonic(), 'direction': 'to_fc', 'hex': data.hex()}) + '\n')
        # Small bounded batches; socket backpressure must fail the run.
        self.sock.settimeout(2)
        self.sock.sendall(data)
        self.sock.setblocking(False)

    def pack(self, message):
        data = message.pack(self.mav)
        self.mav.seq = (self.mav.seq + 1) & 255
        return data

    def send(self, message):
        self.write(self.pack(message))

    def read(self):
        if not select.select([self.sock], [], [], 0)[0]:
            return []
        data = self.sock.recv(65536)
        if not data:
            raise RuntimeError('Simulator closed its UART socket')
        self.capture.write(json.dumps({'time': time.monotonic(), 'direction': 'from_fc', 'hex': data.hex()}) + '\n')
        messages = self.parser.parse_buffer(data) or []
        self.received.extend((time.monotonic(), m) for m in messages)
        return messages


class Bench:
    def __init__(self, base, sensor):
        self.base, self.sensor = base, sensor
        self.next_heartbeat = 0

    def pump(self, seconds, predicate=None):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if time.monotonic() >= self.next_heartbeat:
                self.base.send(mav.MAVLink_heartbeat_message(mav.MAV_TYPE_GCS, mav.MAV_AUTOPILOT_INVALID, 0, 0, 4, 3))
                self.next_heartbeat = time.monotonic() + 1
            self.sensor.read()
            for msg in self.base.read():
                if predicate and predicate(msg):
                    return msg
            time.sleep(.005)
        if predicate:
            raise TimeoutError('Expected simulator response was not received')

    def parameter(self, name):
        self.base.send(mav.MAVLink_param_request_read_message(1, 1, name.encode(), -1))
        return self.pump(5, lambda m: m.get_type() == 'PARAM_VALUE' and m.param_id == name).param_value

    def set_parameter(self, name, value):
        self.base.send(mav.MAVLink_param_set_message(1, 1, name.encode(), value, mav.MAV_PARAM_TYPE_REAL32))
        actual = self.pump(5, lambda m: m.get_type() == 'PARAM_VALUE' and m.param_id == name).param_value
        assert actual == value, (name, actual, value)

    def mode(self, value):
        self.base.send(mav.MAVLink_set_mode_message(1, mav.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, value))
        return self.pump(5, lambda m: m.get_type() == 'HEARTBEAT' and m.get_srcComponent() == 1 and m.custom_mode == value)

    def mission(self):
        self.base.send(mav.MAVLink_mission_request_list_message(1, 1))
        count = self.pump(5, lambda m: m.get_type() == 'MISSION_COUNT').count
        result = []
        for seq in range(count):
            self.base.send(mav.MAVLink_mission_request_int_message(1, 1, seq))
            item = self.pump(5, lambda m: m.get_type() == 'MISSION_ITEM_INT' and m.seq == seq)
            result.append({k: v for k, v in item.to_dict().items() if k not in ('target_system', 'target_component')})
        return result

    def seed_mission(self):
        self.base.send(mav.MAVLink_mission_count_message(1, 1, 2))
        for seq in range(2):
            self.pump(5, lambda m: m.get_type() in ('MISSION_REQUEST', 'MISSION_REQUEST_INT') and m.seq == seq)
            self.base.send(mav.MAVLink_mission_item_int_message(1, 1, seq, mav.MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,
                mav.MAV_CMD_NAV_WAYPOINT, 0, 1, 0, 0, 0, 0, -353632610 + seq * 100, 1491652300, 20))
        ack = self.pump(5, lambda m: m.get_type() == 'MISSION_ACK')
        assert ack.type == mav.MAV_MISSION_ACCEPTED, ack

    def snapshot(self):
        heartbeat = self.pump(3, lambda m: m.get_type() == 'HEARTBEAT' and m.get_srcComponent() == 1)
        return {'custom_mode': heartbeat.custom_mode,
                'armed': bool(heartbeat.base_mode & mav.MAV_MODE_FLAG_SAFETY_ARMED),
                'WPNAV_SPEED': self.parameter('WPNAV_SPEED'), 'mission': self.mission()}


def report(raw, sequence):
    # A real DRID v1 observation with legal Wi-Fi metadata and measured RSSI.
    header = struct.pack('<4sBB6sQIQQ', b'DRID', 1, 1, bytes.fromhex('102030405060'), 1234, sequence, 100, 200)
    record = header + struct.pack('<Bb6sBBBBB25s', 1, -47, bytes.fromhex('020304050607'), 255, 6, 1, 0, 1, raw)
    return record, mav.MAVLink_tunnel_message(255, 190, 0, len(record), record.ljust(128, b'\0'))


def exercise(bench):
    base, sensor = bench.base, bench.sensor
    bench.pump(20, lambda m: m.get_type() == 'HEARTBEAT' and m.get_srcComponent() == 1)
    ready = lambda m: m.get_type() == 'STATUSTEXT' and m.text == 'ArduPilot Ready'
    if not any(ready(m) for _, m in base.received):
        bench.pump(20, ready)
    # Positive controls ensure these state changes are observable and supported
    # on the ordinary trusted port of this same running FC.
    bench.mode(1)
    bench.mode(0)
    bench.set_parameter('WPNAV_SPEED', 432)
    bench.set_parameter('WPNAV_SPEED', 500)
    bench.seed_mission()
    before = bench.snapshot()
    assert len(before['mission']) == 2 and not before['armed']

    delivered = []
    fixtures = [b'\x02\x41\xffP0-SCOUT'.ljust(25, b'\0'), b'\x32\x00P0 hello'.ljust(25, b'\0')]
    for seq, raw in enumerate(fixtures):
        record, msg = report(raw, seq)
        sensor.send(msg)
        routed = bench.pump(5, lambda m: m.get_type() == 'TUNNEL')
        decoded = decode_payload(routed.payload, routed.payload_length)
        expected = decode_payload(record, len(record))
        assert decoded == expected | {'wire_encoding': 'drg2_base64'}
        assert (routed.get_srcSystem(), routed.get_srcComponent()) == (1, 242)
        delivered.append(decoded)
        bench.pump(.3)

    attacks = {
        'set_mode': mav.MAVLink_set_mode_message(1, mav.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, 1),
        'param_set': mav.MAVLink_param_set_message(1, 1, b'WPNAV_SPEED', 137, mav.MAV_PARAM_TYPE_REAL32),
        'mission_clear': mav.MAVLink_mission_clear_all_message(1, 1),
        'mission_upload': mav.MAVLink_mission_count_message(1, 1, 0),
        'command_arm': mav.MAVLink_command_long_message(1, 1, mav.MAV_CMD_COMPONENT_ARM_DISARM, 0, 1, 21196, 0, 0, 0, 0, 0),
        'command_reboot': mav.MAVLink_command_long_message(1, 1, mav.MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 0, 1, 0, 0, 0, 0, 0, 0),
        'rc_override': mav.MAVLink_rc_channels_override_message(1, 1, 1800, 1800, 1000, 1800, 0, 0, 0, 0),
        'fake_gcs_heartbeat': mav.MAVLink_heartbeat_message(mav.MAV_TYPE_GCS, mav.MAV_AUTOPILOT_INVALID, 0, 0, 4, 3),
        'param_list': mav.MAVLink_param_request_list_message(1, 1),
    }
    denied_results = []
    for name, msg in attacks.items():
        for system, component in ((1, 242), (255, 190), (1, 1)):
            tx = mav.MAVLink(None, srcSystem=system, srcComponent=component)
            start = len(base.received)
            sensor.write(msg.pack(tx))
            bench.pump(.3)
            forbidden = {'COMMAND_ACK', 'MISSION_ACK', 'MISSION_REQUEST', 'MISSION_REQUEST_INT', 'PARAM_VALUE'}
            responses = [m.get_type() for _, m in base.received[start:] if m.get_type() in forbidden]
            assert not responses, (name, system, component, responses)
        denied_results.append(name)

    nested = sensor.pack(attacks['set_mode'])
    record, nested_msg = report(nested.ljust(25, b'\0'), 3)
    sensor.send(nested_msg)
    routed = bench.pump(5, lambda m: m.get_type() == 'TUNNEL')
    assert decode_payload(routed.payload, routed.payload_length)['raw_message_hex'] == nested.ljust(25, b'\0').hex()
    bench.pump(.3)
    valid = sensor.pack(report(fixtures[0], 4)[1])
    bad_crc = valid[:-1] + bytes([valid[-1] ^ 1])
    malformed = [bad_crc, valid[:20], b'\xfe\xff' + b'\x00' * 265,
                 bytes([0xfd, 255]) + valid[2:]]
    start = len(base.received)
    for data in malformed:
        sensor.write(data)
        bench.pump(.3)
    assert not [m for _, m in base.received[start:] if m.get_type() == 'TUNNEL']
    after_attacks = bench.snapshot()
    assert after_attacks == before, (before, after_attacks)

    flood_results = []
    for label, packet in (('valid_reports', valid), ('denied_commands', sensor.pack(attacks['set_mode']))):
        start_index = len(base.received)
        started = time.monotonic()
        sent = 0
        # 115200 baud / 10 bits per byte. This emulates pacing, not UART IRQ/DMA.
        while time.monotonic() - started < 3:
            target = int((time.monotonic() - started) * 11520)
            while sent + len(packet) <= target:
                sensor.write(packet)
                sent += len(packet)
            bench.pump(.01)
        duration = time.monotonic() - started
        bench.pump(1)
        messages = base.received[start_index:]
        hbs = [t for t, m in messages if m.get_type() == 'HEARTBEAT' and m.get_srcComponent() == 1]
        tunnels = [m for _, m in messages if m.get_type() == 'TUNNEL']
        flood_results.append({'kind': label, 'bytes_sent': sent, 'seconds': duration,
            'reports_delivered': len(tunnels), 'max_heartbeat_gap_s': max((b-a for a,b in zip(hbs,hbs[1:])), default=None)})
        assert len(hbs) >= 2, 'Lost FC heartbeat during simulator flood'
        if label == 'denied_commands':
            assert not tunnels
        assert bench.snapshot() == before
    sensor.send(report(fixtures[1], 5)[1])
    recovered = bench.pump(5, lambda m: m.get_type() == 'TUNNEL')
    assert decode_payload(recovered.payload, recovered.payload_length)['hello'] == 'P0 hello'
    return {'result': 'passed', 'positive_controls': ['mode', 'parameter', 'mission_upload'],
        'before': before, 'after': bench.snapshot(), 'delivered_reports': delivered,
        'denied_classes': denied_results, 'spoofed_sources': [[1,242],[255,190],[1,1]],
        'malformed_cases': len(malformed), 'nested_command_opaque': True,
        'floods': flood_results, 'recovery_report': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory (must not exist)')
    args = parser.parse_args()
    checkout = args.checkout.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {'scope': 'ArduCopter 4.6.3 SITL, standalone FC filter, unsigned loopback links; no hardware',
        'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'limitations': ['No UART electrical, IRQ/DMA, physical reset or power measurements',
                       'Disarmed only; no armed flight, RC effect or failsafe-authority assertion',
                       'Checks WPNAV_SPEED sentinel, mode and stored mission, not all FC parameters',
                       'Three-second floods are smoke tests; no vehicle latency budget qualified',
                       'External guard, iNav and signed downlink not exercised'],
        'result': 'failed'}
    process = None
    try:
        result['source'] = install('ardupilot', checkout, 'verify')
        with (output/'build.log').open('w') as log:
            for cmd in ([sys.executable, './waf', 'configure', '--board', 'sitl', '--disable-scripting'],
                        [sys.executable, './waf', 'copter', '-j4']):
                subprocess.run(cmd, cwd=checkout, stdout=log, stderr=subprocess.STDOUT, check=True)
        binary = checkout/'build/sitl/bin/arducopter'
        result['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory(prefix='droneid-sitl-') as tmp, \
                socket.socket() as base_listener, socket.socket() as sensor_listener, \
                (output/'sitl.log').open('w') as log, \
                (output/'base.jsonl').open('w') as base_capture, \
                (output/'sensor.jsonl').open('w') as sensor_capture:
            listeners = (base_listener, sensor_listener)
            for listener in listeners:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                listener.settimeout(30)
            defaults = Path(tmp)/'defaults.parm'
            defaults.write_text('SERIAL0_PROTOCOL 2\nSERIAL4_PROTOCOL 2\nSYSID_THISMAV 1\nSYSID_MYGCS 255\nLOG_DISARMED 1\nSIM_SPEEDUP 1\n')
            cmd = [str(binary), '--model', 'quad', '--defaults', str(defaults),
                   '--serial0', f'tcpclient:127.0.0.1:{base_listener.getsockname()[1]}',
                   '--serial4', f'tcpclient:127.0.0.1:{sensor_listener.getsockname()[1]}']
            # This release advertises --serial9 but stores only paths 0..8;
            # specifying 9 overwrites cmdline_param and corrupts startup/exit.
            for index in (1,2,3,5,6,7,8):
                cmd += [f'--serial{index}', 'none']
            process = subprocess.Popen(cmd, cwd=tmp, stdout=log, stderr=subprocess.STDOUT)
            with base_listener.accept()[0] as base_socket, sensor_listener.accept()[0] as sensor_socket:
                result.update(exercise(Bench(Peer(base_socket,255,190,base_capture), Peer(sensor_socket,1,242,sensor_capture))))
    except Exception as exc:
        result['error'] = f'{type(exc).__name__}: {exc}'
        result['result'] = 'failed'
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            result['simulator_exit_code'] = process.returncode
            if process.returncode != 0:
                result['behavior_checks'] = result['result']
                result['result'] = 'failed'
                result['shutdown_error'] = 'Simulator did not exit cleanly'
        (output/'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps({'result': result['result'], 'evidence': str(output)}))
    return 0 if result['result'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
