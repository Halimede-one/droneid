import json
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
import bench
from mavlink_receiver import decode_payload,HEADER


class UARTTests(unittest.TestCase):
    def test_configuration_rejects_collisions_reserved_pins_and_bad_types(self):
        self.assertEqual(bench.uart_command(bench.UART_DEFAULTS),'uart 0 115200 11 12 1 242 1 255 190 5')
        for change in [{'enabled':1},{'system_id':True},{'tx_pin':27},{'rx_pin':11},{'component_id':1},
                       {'baud':9600},{'max_rate':0},{'target_system':256},{'target_system':1,'target_component':242}]:
            with self.assertRaises(ValueError):bench.uart_command(bench.UART_DEFAULTS|change)

    def test_payload_lengths_versions_and_metadata(self):
        header=HEADER.pack(b'DRID',1,1,b'123456',123,1,100,101)
        raw=b'\x32\0Hello UART'+bytes(13)
        self.assertEqual(len(raw),25)
        payload=header+struct.pack('<Bb6sBBBBB25s',1,-50,b'abcdef',255,6,1,0,2,raw)
        decoded=decode_payload(payload,len(payload))
        self.assertEqual(decoded['hello'],'Hello UART');self.assertEqual(decoded['rssi_dbm'],-50)
        self.assertEqual(decoded['session'],'000000000000007b')
        self.assertIsNone(decode_payload(b'other',5))
        for bad in [payload[:10],payload[:-1],payload+b'x',payload[:4]+b'\x02'+payload[5:],
                    payload[:40]+b'\x03'+payload[41:],payload[:52]+b'\0'+payload[53:]]:
            with self.assertRaises(ValueError):decode_payload(bad,len(bad))
        with self.assertRaises(ValueError):decode_payload(payload,129)

    def test_c_frames_decode_with_reference_pymavlink(self):
        fixtures=os.environ.get('DRONEID_FC_FIXTURES')
        if not fixtures:self.skipTest('Run tests/run_host_tests.sh for C fixtures')
        try:from pymavlink.dialects.v20 import common
        except ImportError:self.skipTest('Install tools/requirements-mavlink.txt for reference decoder test')
        frames=[json.loads(line) for line in Path(fixtures).read_text().splitlines()]
        parser=common.MAVLink(None)
        messages=[parser.parse_char(bytes.fromhex(f['frame_hex'])) for f in frames]
        self.assertEqual(messages[0].get_type(),'HEARTBEAT')
        self.assertEqual(messages[0].type,common.MAV_TYPE_ONBOARD_CONTROLLER)
        for i in (1,2):
            msg=messages[i];self.assertEqual(msg.get_type(),'TUNNEL')
            self.assertEqual((msg.get_srcSystem(),msg.get_srcComponent()),(1,242))
            self.assertEqual((msg.target_system,msg.target_component,msg.payload_type),(255,190,0))
        observation=decode_payload(messages[1].payload,messages[1].payload_length)
        self.assertEqual(observation['broadcast_id'],'SCOUT-01')
        self.assertEqual(observation['rssi_dbm'],-47)
        self.assertEqual(observation['receiver_id'],'10bda3ca3860')
        self.assertEqual(observation['session'],'123456789abcdef0')
        self.assertEqual((observation['rx_us'],observation['reported_us'],observation['seq']),(1000000,1001234,42))
        health=decode_payload(messages[2].payload,messages[2].payload_length)
        self.assertEqual(health['uart_queue_drops'],1);self.assertIsNone(health['fc_heartbeat_age_ms'])


if __name__=='__main__':unittest.main()
