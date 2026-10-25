import importlib.util
import json
import os
import sys
from pathlib import Path
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from usb_fixtures import scout, reply

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))

spec = importlib.util.spec_from_file_location('bench', Path(__file__).parents[1] / 'tools/bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class StreamTests(unittest.TestCase):
    def test_command_recovers_from_role_restart(self):
        class Disconnected(Exception):
            pass

        class Device:
            def __init__(self, fail=False, ok=True):
                self.fail, self.ok = fail, ok

            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def write(self, data):
                self.command = data

            def read(self, size):
                if self.fail:
                    raise Disconnected('USB re-enumerating')
                token = self.command.decode().split()[-1]
                return (json.dumps(reply(request_id=token,ok=self.ok,message='rejected')) + '\n').encode()

        first, second = Device(fail=True), Device()
        with patch.object(bench, 'serial_modules', return_value=(SimpleNamespace(SerialException=Disconnected), None)), \
             patch.object(bench, 'connect', side_effect=[first, second]) as connect, \
             patch.object(bench.time, 'sleep'):
            self.assertTrue(bench.request('/stable/board', 'transport wifi')['ok'])
            self.assertEqual(first.command, second.command)
            self.assertEqual(connect.call_count, 2)
            self.assertTrue(all(call.args == ('/stable/board',) for call in connect.call_args_list))
        with patch.object(bench, 'serial_modules', return_value=(SimpleNamespace(SerialException=Disconnected), None)), \
             patch.object(bench, 'connect', return_value=Device(ok=False)) as connect:
            with self.assertRaisesRegex(RuntimeError, 'rejected'):
                bench.request('/stable/board', 'transport both')
            self.assertEqual(connect.call_count, 1)

    def test_fragmented_and_multiple_records(self):
        decoder = bench.LineDecoder()
        encoded = (json.dumps(scout())+'\n').encode()
        self.assertEqual(decoder.feed(encoded[:10]), [])
        self.assertEqual(len(decoder.feed(encoded[10:] + encoded)), 2)

    def test_boot_noise_and_partial_reconnect(self):
        decoder = bench.LineDecoder()
        records = decoder.feed(b'ESP-ROM boot\npartial}\n'+(json.dumps(reply())+'\n').encode())
        self.assertEqual(len(records), 1)
        self.assertEqual(decoder.invalid_lines, 2)

    def test_oversized_line_cannot_be_resumed_as_a_record(self):
        decoder = bench.LineDecoder(limit=500)
        encoded = (json.dumps(scout())+'\n').encode()
        self.assertEqual(decoder.feed(b'x' * 501 + encoded), [])
        self.assertEqual(len(decoder.feed(encoded)), 1)
        self.assertLessEqual(len(decoder.buffer), 500)

    def test_invalid_schema_json_and_rssi(self):
        decoder = bench.LineDecoder()
        for line in (b'null', b'[]', b'{"schema":2,"kind":"status"}',
                     b'{"schema":true,"kind":"status"}',
                     b'{"schema":1,"kind":"observation","rssi_dbm":true}',
                     b'{"schema":1,"kind":"observation","rssi_dbm":-200}',
                     b'{"schema":1,"kind":"status","value":NaN}', b'\xff'):
            self.assertEqual(decoder.feed(line + b'\n'), [])

    def test_real_c_serializer_fixtures(self):
        fixture_path = os.environ.get('DRONEID_TEST_FIXTURES')
        if not fixture_path:
            self.skipTest('Run tests/run_host_tests.sh for C serializer fixtures')
        records = bench.LineDecoder().feed(Path(fixture_path).read_bytes())
        self.assertEqual(len(records), 10)
        basic, experimental, self_id, unknown_location, location, future, escaped, system, wifi, nan = records
        self.assertEqual(basic['data']['uas_id_text'], 'TEST-DRONE')
        self.assertEqual(basic['rssi_dbm'], -57)
        self.assertEqual(basic['source_address'], '01:02:03:04:05:06')
        self.assertEqual((basic['rx_us'], basic['reported_us'], basic['seq']), (1000, 2000, 42))
        self.assertTrue(experimental['data']['uas_id_hex'].startswith('ff42454e43482d'))
        self.assertIsNone(experimental['data']['uas_id_text'])
        self.assertEqual(self_id['data']['description'], 'BENCH TEST - NO FLIGHT')
        self.assertIsNone(unknown_location['rssi_dbm'])
        for field in ('latitude_deg', 'longitude_deg', 'altitude_geodetic_m',
                      'horizontal_speed_mps', 'direction_deg', 'seconds_after_hour'):
            self.assertIsNone(unknown_location['data'][field], field)
        self.assertEqual(location['data']['latitude_deg'], 37.5)
        self.assertEqual(location['data']['longitude_deg'], -122.25)
        self.assertEqual(location['data']['altitude_geodetic_m'], 12.5)
        self.assertAlmostEqual(location['data']['seconds_after_hour'], 123.4, places=4)
        self.assertEqual(future['decode_status'], 'unsupported')
        self.assertIsNone(future['data'])
        self.assertEqual(escaped['data']['description'], 'quote" slash\\ newline\n')
        self.assertIsNone(system['data']['operator_latitude_deg'])
        self.assertEqual((wifi['transport'], wifi['channel'], wifi['frequency_mhz']),
                         ('wifi_beacon', 6, 2437))
        self.assertEqual((nan['transport'], nan['channel'], nan['frequency_mhz']),
                         ('wifi_nan', 149, 5745))
        self.assertEqual((wifi['pack_index'], nan['pack_index'], nan['pack_size']), (0, 1, 2))
        self.assertIsNone(wifi['source_address_type'])
        self.assertIsNone(wifi['phy'])
        self.assertEqual(wifi['data']['uas_id_text'], 'TEST-DRONE')
        self.assertEqual(nan['data']['description'], 'BENCH TEST - NO FLIGHT')
        self.assertIn('wifi_nan/ch149', bench.human_record(nan))
        for record in records:
            self.assertEqual(len(bytes.fromhex(record['raw_message_hex'])), 25)
            self.assertEqual(record['authentication'], 'unverified')
            self.assertIsInstance(bench.human_record(record), str)


if __name__ == '__main__':
    unittest.main()
