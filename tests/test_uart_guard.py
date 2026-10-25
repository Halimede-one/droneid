import base64
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from mavlink_receiver import decode_payload, HEADER
import dashboard


class GuardTests(unittest.TestCase):
    def payload(self):
        header = HEADER.pack(b'DRID', 1, 1, b'123456', 123, 1, 100, 101)
        raw = b'\x32\0Hello guard' + bytes(12)
        return header + struct.pack('<Bb6sBBBBB25s', 1, -51, b'abcdef', 255, 6, 1, 0, 1, raw)

    def test_guard_encoding_is_decoded_once_without_implying_authentication(self):
        raw = self.payload()
        wrapper = b'DRG2' + base64.b64encode(raw)
        self.assertEqual(len(wrapper), 108)
        original = decode_payload(raw, len(raw))
        guarded = decode_payload(wrapper, len(wrapper))
        self.assertEqual(guarded, original | {'wire_encoding': 'drg2_base64'})
        self.assertEqual(guarded['authentication'], 'unverified')
        for bad in (wrapper[:-1], wrapper+b'x', b'DRG2'+b'!'*104,
                    b'DRG2'+base64.b64encode(b'DRG2'+raw[4:]),
                    b'DRG2'+base64.b64encode(raw[:4]+b'\x02'+raw[5:])):
            with self.assertRaises(ValueError):
                decode_payload(bad, len(bad))

    def test_guard_board_cannot_be_flashed_with_radio_bundle_after_identification(self):
        with tempfile.TemporaryDirectory() as directory:
            info = {'id': '102030405060', 'serial': '10:20:30:40:50:60', 'port': '/offline', 'tty': '/offline'}
            device = dashboard.Device(info, log_dir=directory, start=False)
            try:
                device.record({'schema':1, 'kind':'status', 'receiver_id': info['id'],
                               'session':'guard', 'device_type':'uart_guard'})
                bundle = Mock()
                with self.assertRaisesRegex(ValueError, 'UART guard'):
                    device.flash(bundle, 'irrelevant')
                device.status = None  # Reconnect must not silently forget its role.
                self.assertEqual(device.snapshot()['device_type'], 'uart_guard')
                with self.assertRaisesRegex(ValueError, 'UART guard'):
                    device.flash(bundle, 'irrelevant')
                bundle.bundle.assert_not_called()
            finally:
                device.log_handler.close()

    def test_c_output_with_reference_mavlink_and_base_station_decoder(self):
        path = os.environ.get('DRONEID_GUARD_FIXTURES')
        if not path:
            self.skipTest('Run tests/run_host_tests.sh for C fixtures')
        try:
            from pymavlink.dialects.v20 import common
        except ImportError:
            self.skipTest('Install tools/requirements-mavlink.txt for reference decoder test')
        frames = [bytes.fromhex(json.loads(line)['frame_hex']) for line in Path(path).read_text().splitlines()]
        messages = []
        for frame in frames:
            self.assertEqual(frame[0], 0xfd)
            self.assertFalse(any(b in (0xfd, 0xfe) for b in frame[1:]))
            message = common.MAVLink(None).parse_char(frame)
            self.assertEqual((message.get_srcSystem(), message.get_srcComponent()), (1, 243))
            messages.append(message)
        self.assertEqual(messages[0].get_type(), 'HEARTBEAT')
        self.assertEqual(messages[0].autopilot, common.MAV_AUTOPILOT_INVALID)
        for message in messages[1:]:
            self.assertEqual(message.get_type(), 'TUNNEL')
            self.assertEqual((message.target_system, message.target_component, message.payload_type), (255,190,0))
        observation = decode_payload(messages[1].payload, messages[1].payload_length)
        self.assertEqual(observation['hello'], 'Hello guard')
        self.assertEqual(observation['rssi_dbm'], -51)
        self.assertEqual(observation['receiver_id'], '102030405060')
        self.assertEqual(observation['authentication'], 'unverified')
        health = decode_payload(messages[2].payload, messages[2].payload_length)
        self.assertEqual(health['uart_queue_drops'], 1)
        self.assertIsNone(health['fc_heartbeat_age_ms'])


if __name__ == '__main__':
    unittest.main()
