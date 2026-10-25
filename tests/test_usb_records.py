import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]/'tools'))
import bench
import dashboard
import server
import usb_records
from usb_fixtures import scout, guard, observation, reply


def wire(value):
    return (json.dumps(value)+'\n').encode()


def uart():
    return dict(bench.UART_DEFAULTS,configuration_version=1,ready=True,connected=True,
        heartbeat_age_ms=0,position_age_ms=1,gps_age_ms=-1,
        position=dict(lat_e7=375000000,lon_e7=-1222500000,altitude_msl_mm=1000,relative_altitude_mm=500),
        gps=dict(fix_type=3,satellites=8),tx_frames=1,rx_frames=1,queue_drops=0,expired=0,
        parse_errors=0,signed_ignored=0,errors=0)


class SchemaTests(unittest.TestCase):
    def test_current_records_optional_telemetry_and_limits(self):
        records=[scout(),scout(uart=None),scout(uart=uart(),rx_transports=['ble_legacy','wifi_beacon','wifi_nan']),
                 guard(),reply(),observation(),observation('self_id'),
                 scout(uptime_us=2**64-1,free_heap_bytes=2**32-1)]
        decoder=bench.LineDecoder()
        self.assertEqual(decoder.feed(b''.join(map(wire,records))),records)
        self.assertEqual(decoder.invalid_lines,0)

    def test_types_ranges_nested_schemas_and_recovery(self):
        valid=scout(uart=uart())
        bad=[scout(schema=True),scout(session=[]),scout(kind={}),scout(role=[]),scout(firmware={}),
             scout(radio_ready=1),scout(uptime_us=-1),scout(uptime_us=1.5),scout(uptime_us=2**64),
             scout(free_heap_bytes=True),scout(free_heap_bytes=2**32),scout(request_id=[]),
             scout(hello_hex='zz'),scout(hello_hex='1b'),scout(broadcast_id='A'*20),
             scout(rx_transports=[{}]),scout(rx_transports=['wifi_beacon']*4),scout(uart=[]),
             scout(uart=uart()|{'baud':True}),scout(uart=uart()|{'position':[]}),
             scout(uart=uart()|{'position':dict(uart()['position'],lat_e7=900000001)}),
             scout(uart=uart()|{'gps':{'fix_type':{},'satellites':8}}),
             scout(uart=uart()|{'heartbeat_age_ms':-2}),scout(uart=uart()|{'tx_pin':12}),
             guard(sensor={'input_bytes':-1}),guard(fc=[]),guard(radios_enabled='false'),guard(ready=1),
             guard(board_id='xiao_esp32s3'),reply(ok='false'),reply(message='\x1b[2J'),
             observation(data=[]),observation(data={'uas_id_text':'poison'}),observation(rssi_dbm=True),
             observation(rx_us=101,reported_us=100),observation(pack_index=1,pack_size=1),
             observation(raw_message_hex='ff'),observation(authentication='verified'),
             observation('self_id',data={'description_type':0,'description':'x'*24}),
             scout(unknown={'deep':'data'}),scout(broadcast_id='\ud800')]
        for record in bad:
            with self.subTest(record=record):
                decoder=bench.LineDecoder()
                self.assertEqual(decoder.feed(wire(record)+wire(valid)),[valid])
                self.assertEqual(decoder.invalid_lines,1)
                self.assertTrue(decoder.last_error)

    def test_numeric_overflow_duplicates_encoding_and_structural_limits(self):
        good=wire(scout())
        prefix=b'{"schema":1,"kind":"status","free_heap_bytes":'
        bad=[prefix+n+b'}' for n in (b'1e309',b'-1e309',b'NaN',b'Infinity',b'-Infinity',b'9'*3000)]
        duplicate=wire(scout()).replace(b'"radio_ready": true',b'"radio_ready": false, "radio_ready": true').rstrip()
        nested_duplicate=wire(scout(uart=uart())).replace(b'"fix_type": 3',b'"fix_type": 2, "fix_type": 3').rstrip()
        bad += [duplicate,nested_duplicate,b'['*1000+b'0'+b']'*1000,good.decode().encode('utf-16'),
                b'\xff',b'{"data":"'+b'x'*5000+b'"}']
        for raw in bad:
            with self.subTest(size=len(raw),prefix=raw[:50]):
                decoder=bench.LineDecoder()
                # Fragmented over-limit and nested input must recover only at newline.
                self.assertEqual(decoder.feed(raw[:20]),[])
                self.assertEqual(decoder.feed(raw[20:]+b'\n'+good),[scout()])
                self.assertGreaterEqual(decoder.invalid_lines,1)
                self.assertLessEqual(len(decoder.buffer),4096)
        for value in (float('nan'),float('inf'),float('-inf')):
            with self.assertRaises(ValueError):usb_records.snapshot(scout(uptime_us=value))
        with self.assertRaises(ValueError):
            usb_records.snapshot({'nested':[None]*300})

    def test_rejected_record_does_not_mutate_state_and_accepted_record_is_copied(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=dashboard.Device(dict(id='102030405060',serial='102030405060',port='/fake',tty='/fake'),log_dir=tmp,start=False)
            try:
                source=scout(uart=uart());d.record(source);d.record(observation())
                baseline=copy.deepcopy((d.status,d.session,d.contacts.items,list(d.logs),d.log_cursor))
                for record in (scout(session='new',uptime_us=float('inf')),scout(session='new',uart=[]),
                               observation(session='new',data=[]),reply(session='new',ok={})):
                    with self.assertRaises(ValueError):d.record(record)
                    self.assertEqual((d.status,d.session,d.contacts.items,list(d.logs),d.log_cursor),baseline)
                    json.dumps(d.snapshot(),allow_nan=False)
                source['uart']['position']['lat_e7']=float('inf')
                self.assertEqual(d.status['uart']['position']['lat_e7'],375000000)
            finally:d.log_handler.close()


class ReaderIsolationTests(unittest.TestCase):
    def api_state(self, devices):
        manager=SimpleNamespace(token='test',snapshot=lambda:{'devices':[d.snapshot() for d in devices]},
                                firmware=SimpleNamespace(info=lambda:{'available':False}))
        base=server.handler_for(manager);output=io.BytesIO()
        class Handler(base):
            def setup(self):
                self.rfile=io.BytesIO(b'GET /api/state HTTP/1.1\r\nHost: 127.0.0.1:8766\r\nConnection: close\r\n\r\n')
                self.wfile=output
            def finish(self):pass
        Handler(None,('127.0.0.1',1234),SimpleNamespace(server_port=8766))
        response=output.getvalue()
        self.assertEqual(int(response.split()[1]),200)
        return json.loads(response.split(b'\r\n\r\n',1)[1])

    def wait_for(self, predicate):
        deadline=time.monotonic()+3
        while not predicate() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue(predicate())

    def test_bad_device_cannot_break_state_api_healthy_device_or_its_own_recovery(self):
        release=threading.Event()
        bad_key,good_key='102030405060','102030405061'
        malformed=[wire(scout(free_heap_bytes=1e309)),wire(scout(uart=[])),wire(observation(data=[])),
                   b'{"schema":1,"schema":1}\n',b'['*1000+b'0'+b']'*1000+b'\n',b'x'*4097+b'\n']
        class Stream:
            def __init__(self,path):self.key=path.split('/')[-1];self.reads=0
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def write(self,data):pass
            def read(self,size):
                time.sleep(.005);self.reads+=1
                if self.key==bad_key and not release.is_set():
                    return malformed[self.reads-1] if self.reads<=len(malformed) else b''
                return wire(scout(receiver_id=self.key,uptime_us=self.reads))
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(bench,'serial_modules',return_value=(SimpleNamespace(SerialException=OSError),SimpleNamespace(comports=lambda:[]))), \
             patch.object(bench,'connect',side_effect=Stream):
            devices=[]
            for key in (bad_key,good_key):
                info=dict(id=key,serial=key,port='/fake/'+key,tty='/fake/'+key)
                devices.append(dashboard.Device(info,locator=lambda info=info:{info['id']:info},log_dir=tmp,start=True))
            bad,good=devices
            try:
                self.wait_for(lambda:bad.usb_input['rejected_lines']==len(malformed) and good.status is not None)
                self.assertTrue(bad.thread.is_alive() and good.thread.is_alive())
                self.assertIsNone(bad.status);self.assertIsNotNone(bad.error)
                previous=good.status['uptime_us']
                self.api_state(devices)
                release.set()
                self.wait_for(lambda:bad.status is not None and good.status['uptime_us']>previous)
                self.assertIsNone(bad.error)
                self.assertEqual(bad.usb_input['rejected_lines'],len(malformed))
                self.assertEqual(good.usb_input['rejected_lines'],0)
                self.api_state(devices)
            finally:
                for d in devices:d.stop.set()
                for d in devices:d.thread.join(3)

    def test_unexpected_processor_exception_releases_port_and_reader_recovers(self):
        opened=[]
        class Stream:
            def __enter__(self):opened.append(self);return self
            def __exit__(self,*args):pass
            def write(self,data):pass
            def read(self,size):time.sleep(.005);return wire(scout())
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(bench,'serial_modules',return_value=(SimpleNamespace(SerialException=OSError),SimpleNamespace(comports=lambda:[]))), \
             patch.object(bench,'connect',side_effect=lambda p:Stream()):
            info=dict(id='102030405060',serial='102030405060',port='/fake',tty='/fake')
            d=dashboard.Device(info,locator=lambda:{info['id']:info},log_dir=tmp,start=False)
            original=d.record
            calls=0
            def fail_once(record):
                nonlocal calls
                calls+=1
                if calls==1:raise TypeError('unexpected processor bug')
                return original(record)
            d.record=fail_once
            try:
                d.thread.start()
                self.wait_for(lambda:d.status is not None)
                self.assertTrue(d.thread.is_alive())
                self.assertEqual(d.usb_input['reader_failures'],1)
                self.assertIn('TypeError',d.usb_input['last_error'])
                self.assertGreaterEqual(len(opened),2)
                self.api_state([d])
            finally:d.stop.set();d.thread.join(3)
