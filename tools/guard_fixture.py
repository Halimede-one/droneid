"""Bounded bench-only UART smoke test using two explicitly selected USB adapters.

No flight controller is attached to this fixture. USB on the guard stays diagnostic.
This checks allowed traffic in both directions and a small rejection sample;
it is not the sustained-load/reset/electrical flight acceptance test.
"""
from contextlib import ExitStack
import hashlib
from importlib.util import find_spec
import secrets
import time
import bench

def adapters():
    _, ports = bench.serial_modules()
    found = {}
    for p in ports.comports():
        # Native ESP ports are DUTs, never fixture adapters. Serial identity
        # must survive tty renumbering; duplicate adapter identities are omitted.
        if p.vid not in (0x0403,0x10c4,0x1a86,0x067b) or not p.serial_number:
            continue
        identity = f'{p.vid:04x}:{p.pid:04x}:{p.serial_number}:{p.location or ""}'
        key = hashlib.sha256(identity.encode()).hexdigest()[:20]
        if key in found:
            found[key] = None
        else:
            found[key] = {'id':key,'port':p.device,'label':f'{p.description} · {p.serial_number} · {p.device}'}
    return {k:v for k,v in found.items() if v}

def info():
    if find_spec('serial') is None:
        return {'available':False, 'adapters':[], 'error':'Install tools/requirements.txt.'}
    try:
        return {'available':find_spec('pymavlink') is not None, 'adapters':list(adapters().values())}
    except (ImportError, OSError) as exc:
        return {'available':False, 'adapters':[], 'error':str(exc)}

def run(sensor_id, fc_id, log, system=1, sensor_component=242, fc_component=1,
        guard_component=243, target_system=255, target_component=190):
    if not isinstance(sensor_id,str) or not isinstance(fc_id,str) or sensor_id == fc_id:
        raise ValueError('Choose two different USB UART adapters.')
    from pymavlink.dialects.v20 import common as mav
    current = adapters()
    if sensor_id not in current or fc_id not in current:
        raise ValueError('Selected fixture adapter disconnected or changed. Refresh and retry.')
    sensor_path, fc_path = current[sensor_id]['port'], current[fc_id]['port']
    if sensor_path == fc_path:
        raise ValueError('Fixture ports must differ.')
    with ExitStack() as stack:
        sensor = stack.enter_context(bench.connect(sensor_path))
        fc = stack.enter_context(bench.connect(fc_path))
        for port in (sensor,fc):
            port.timeout = 0
            port.reset_input_buffer()
        tx = mav.MAVLink(None,srcSystem=system,srcComponent=sensor_component)
        controller = mav.MAVLink(None,srcSystem=system,srcComponent=fc_component)
        parsers = [mav.MAVLink(None), mav.MAVLink(None)]
        for parser in parsers:
            parser.robust_parsing = True
        nonce = secrets.token_bytes(6)
        payload = bytearray(128)
        payload[:6] = b'DRID\x01\x01'
        payload[6:12] = nonce
        payload[41] = 206  # -50 dBm; BLE channel 0, address type 0.
        payload[52] = 1
        payload[53] = 0x32
        expected = bytes(payload[:78])
        allowed = mav.MAVLink_tunnel_message(target_system,target_component,0,78,payload).pack(tx)
        denied = mav.MAVLink_param_request_list_message(system,fc_component).pack(tx)
        corrupt = bytearray(allowed); corrupt[-1] ^= 1
        heartbeat = mav.MAVLink_heartbeat_message(mav.MAV_TYPE_QUADROTOR,mav.MAV_AUTOPILOT_ARDUPILOTMEGA,0,0,mav.MAV_STATE_ACTIVE,3).pack(controller)
        reverse_denied = mav.MAVLink_param_request_list_message(system,sensor_component).pack(controller)
        import base64
        seen_report = seen_hb = False
        start = time.monotonic()
        next_send = start
        log('Fixture active: valid observation + FC heartbeat, forbidden parameter requests and bad CRC. Duration 6 seconds.')
        while time.monotonic()-start < 6:
            now = time.monotonic()
            if now >= next_send:
                sensor.write(allowed+denied+corrupt)
                fc.write(heartbeat+reverse_denied)
                next_send = now+1
            for side, port in enumerate((sensor,fc)):
                for msg in parsers[side].parse_buffer(port.read(1024)) or []:
                    name = msg.get_type()
                    if name == 'BAD_DATA':
                        if now-start < .25:
                            continue  # Adapter may have opened mid-heartbeat.
                        raise RuntimeError('Malformed bytes on fixture output; check wiring and UART errors.')
                    if side == 0:
                        if name != 'HEARTBEAT' or msg.get_srcSystem()!=system or msg.get_srcComponent()!=fc_component or msg.autopilot!=mav.MAV_AUTOPILOT_ARDUPILOTMEGA:
                            raise RuntimeError('Unexpected frame on guard sensor output: '+name)
                        seen_hb = True
                    else:
                        if any(b in (0xfd,0xfe) for b in msg.get_msgbuf()[1:]):
                            raise RuntimeError('Nested frame marker on guard FC output.')
                        if name not in ('HEARTBEAT','TUNNEL') or msg.get_srcSystem()!=system or msg.get_srcComponent()!=guard_component:
                            raise RuntimeError('Unexpected frame on guard FC output: '+name)
                        if name == 'HEARTBEAT' and (msg.type!=mav.MAV_TYPE_ONBOARD_CONTROLLER or msg.autopilot!=mav.MAV_AUTOPILOT_INVALID or msg.base_mode or msg.custom_mode):
                            raise RuntimeError('Guard heartbeat has unexpected authority fields.')
                        if name == 'TUNNEL':
                            raw = bytes(msg.payload[:msg.payload_length])
                            if msg.target_system!=target_system or msg.target_component!=target_component or msg.payload_type!=0 or not raw.startswith(b'DRG2'):
                                raise RuntimeError('Invalid wrapped report on guard FC output.')
                            if base64.b64decode(raw[4:],validate=True) != expected:
                                raise RuntimeError('Report nonce/data did not match this fixture run.')
                            seen_report = True
            time.sleep(.01)
        if not seen_report or not seen_hb:
            raise RuntimeError('Missing '+('sensor → FC report' if not seen_report else 'FC → sensor heartbeat')+'. Check TX/RX, GND and selected adapters.')
        log('PASS: bidirectional UART smoke test; no forbidden parameter requests observed. UART load/reset/electrical acceptance remains open.')
