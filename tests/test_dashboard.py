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

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import bench
import dashboard
import server


def observation(kind='basic_id', **changes):
    value = {'schema':1,'kind':'observation','session':'boot1','source_address':'10:20:30:40:50:60',
             'source_address_type':0,'transport':'wifi_beacon','channel':6,'rssi_dbm':-55,
             'message_type':kind,'decode_status':'ok','rx_us':100,'reported_us':100,
             'data':{'id_type':4,'uas_id_hex':(b'\xffSCOUT-01'+b'\0'*11).hex()}}
    value.update(changes)
    return value


class AggregationTests(unittest.TestCase):
    def test_messages_in_one_wifi_pack_are_not_dropped_by_host_timing(self):
        c=dashboard.Contacts()
        c.ingest(observation(rx_us=1000,reported_us=2000),100)
        c.ingest(observation('self_id',rx_us=1000,reported_us=3000,data={'description':'Same packet'}),100.0001)
        self.assertEqual(c.snapshot(101)[0]['hello'],'Same packet')
        self.assertEqual(c.snapshot(101)[0]['count'],2)
        c.ingest(observation('self_id',rx_us=999,reported_us=4000,data={'description':'Older'}),102)
        self.assertEqual(c.snapshot(103)[0]['hello'],'Same packet')

    def test_fields_age_independently_and_identity_changes_clear_hello(self):
        c=dashboard.Contacts()
        c.ingest(observation(),100)
        c.ingest(observation('self_id',data={'description':'Hello'}),102)
        c.ingest(observation(),110)
        contact=c.snapshot(112)[0]
        self.assertEqual((contact['id'],contact['hello']),('SCOUT-01','Hello'))
        self.assertEqual((contact['age_s'],contact['id_age_s'],contact['hello_age_s']),(2,2,10))
        c.ingest(observation(data={'uas_id_text':'DIFFERENT'}),113)
        self.assertIsNone(c.snapshot(114)[0]['hello'])
        self.assertEqual(c.snapshot(240),[])

    def test_sources_do_not_merge_on_claimed_id_and_wifi_formats_do(self):
        c=dashboard.Contacts()
        c.ingest(observation(),100)
        c.ingest(observation(transport='wifi_nan'),101)
        c.ingest(observation(source_address='aa:bb:cc:dd:ee:ff'),102)
        self.assertEqual(len(c.snapshot(103)),2)
        self.assertEqual(sorted(x['count'] for x in c.snapshot(103)),[1,2])

    def test_backlog_unknown_values_bounded_memory_and_untrusted_text(self):
        c=dashboard.Contacts()
        c.ingest(observation(rx_us=0,reported_us=9000000),100)
        c.ingest(observation('self_id',data={'description':'<svg onload=alert(1)>'}),101)
        self.assertEqual(c.snapshot(102)[0]['id_age_s'],11)
        self.assertEqual(c.snapshot(102)[0]['hello'],'<svg onload=alert(1)>')
        c.ingest(observation(data=None,rssi_dbm=None),103)
        self.assertEqual(c.snapshot(104)[0]['rssi_dbm'],-55)
        for i in range(150):
            c.ingest(observation(source_address=f'10:20:30:40:50:{i:02x}'),104+i/1000)
        self.assertEqual(len(c.items),dashboard.MAX_CONTACTS)
        c.ingest(observation(source_address='bad-address'),105)
        self.assertEqual(len(c.items),dashboard.MAX_CONTACTS)


class ConfigurationTests(unittest.TestCase):
    def test_ascii_limits_and_transport(self):
        cmd=bench.configuration_command('both','wifi','SCOUT-01','Hello "friend" \\')
        parts=cmd.split()
        self.assertEqual(bytes.fromhex(parts[3]).decode(),'SCOUT-01')
        self.assertEqual(bytes.fromhex(parts[4]).decode(),'Hello "friend" \\')
        self.assertTrue(bench.configuration_command('rx','ble','A'*19,'!'*23))
        self.assertTrue(bench.configuration_command('rx','wifi','A','').endswith(' -'))
        for args in [('both','both','A',''),('rx','wifi','A'*20,''),('rx','wifi','',''),
                     ('rx','wifi','<svg>',''),('rx','wifi','A','\n'),('rx','wifi','A','é'),
                     ('rx','wifi','A','!'*24),('rx','wifi',None,'')]:
            with self.assertRaises(ValueError):bench.configuration_command(*args)


class FirmwareTests(unittest.TestCase):
    def test_bundle_restricts_target_paths_and_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for name in ('boot.bin','partition.bin','app.bin'):(root/name).write_bytes(b'firmware')
            manifest={'extra_esptool_args':{'chip':'esp32c5'},'flash_files':{'0x2000':'boot.bin','0x8000':'partition.bin','0x10000':'app.bin'}}
            (root/'flasher_args.json').write_text(json.dumps(manifest))
            (root/'project_description.json').write_text(json.dumps({'target':'esp32c5','project_name':'droneid','project_version':'test'}))
            firmware=dashboard.Firmware(root)
            old=firmware.bundle()['fingerprint']
            (root/'app.bin').write_bytes(b'changed')
            self.assertNotEqual(old,firmware.bundle()['fingerprint'])
            for mutate in (lambda m:m['extra_esptool_args'].update(chip='esp32'),
                           lambda m:m['flash_files'].update({'0x9000':'app.bin'}),
                           lambda m:m['flash_files'].update({'0x10000':'../outside.bin'})):
                changed=copy.deepcopy(manifest);mutate(changed)
                (root/'flasher_args.json').write_text(json.dumps(changed))
                self.assertFalse(firmware.info()['available'])


class DeviceTests(unittest.TestCase):
    def device(self,tmp,key='102030405060',start=False):
        info={'id':key,'serial':key,'port':'/stable/'+key,'tty':'/dev/example'}
        return dashboard.Device(info,locator=lambda:{key:info},log_dir=tmp,start=start)

    def test_identify_requires_supported_present_board_and_serializes_operations(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=self.device(tmp)
            try:
                with self.assertRaises(ValueError): d.identify()
                d.status={'identify_supported':True}
                d.present=False
                with self.assertRaises(RuntimeError): d.identify()
                d.present=True
                d.operation_lock.acquire()
                with self.assertRaises(RuntimeError): d.identify()
                d.operation_lock.release()
                self.assertIsNone(d.identify_request)
            finally:d.log_handler.close()

    def test_identify_uses_existing_reader_and_requires_active_status(self):
        # Opening a new serial stream simulates a board reset. An Identify ACK
        # alone must never count as success, and the reader must remain open.
        for mode in ('ok','inactive','reboot','rejected','disconnect','paused'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                opened=[]
                class Stream:
                    def __init__(self,port):
                        self.port=port;self.pending=[];self.active=False;self.closed=False
                    def __enter__(self):opened.append(self);return self
                    def __exit__(self,*args):self.closed=True;self.active=False
                    def emit(self,kind,token='',**extra):
                        self.pending.append(dict(schema=1,kind=kind,receiver_id=self.port.split('/')[-1],
                            session='boot'+str(len(opened)),request_id=token,**extra))
                    def write(self,value):
                        for line in value.decode().splitlines():
                            parts=line.split()
                            if not parts:continue
                            token=parts[1] if len(parts)>1 else ''
                            if parts[0]=='identify':
                                if mode=='disconnect':raise OSError('USB lost')
                                self.active=mode not in ('inactive','rejected')
                                self.emit('reply',token,ok=mode!='rejected',message='rejected')
                                if mode=='reboot':self.pending[-1]['session']='previous-boot'
                            else:
                                self.emit('status',token,identify_supported=True,identifying=self.active)
                    def read(self,size):
                        time.sleep(.01)
                        data=b''.join((json.dumps(r)+'\n').encode() for r in self.pending)
                        self.pending=[]
                        return data
                with patch.object(bench,'serial_modules',return_value=(SimpleNamespace(SerialException=OSError),None)), \
                     patch.object(bench,'connect',side_effect=Stream) as connect:
                    d=self.device(tmp)
                    try:
                        if mode=='paused':
                            d.monitoring=False;d.status={'identify_supported':True}
                        d.thread.start()
                        if mode!='paused':
                            deadline=time.monotonic()+2
                            while not d.status and time.monotonic()<deadline:time.sleep(.01)
                            self.assertTrue(d.status)
                        if mode in ('ok','paused'):
                            status=d.identify()
                            self.assertTrue(status['identifying'])
                            self.assertEqual(status['session'],'boot1')
                            time.sleep(.05)
                            self.assertFalse(opened[0].closed)
                            self.assertTrue(d.wants_usb())
                            self.assertEqual(d.monitoring,mode!='paused')
                        else:
                            message={'inactive':'did not enter Identify', 'reboot':'restarted',
                                     'rejected':'rejected', 'disconnect':'USB monitoring stopped'}[mode]
                            with self.assertRaisesRegex(RuntimeError,message):d.identify()
                        self.assertEqual(connect.call_count,1)
                        self.assertIsNone(d.identify_request)
                        self.assertFalse(d.operation_lock.locked())
                        self.assertFalse(d.pause.is_set())
                        self.assertTrue(any(x['record']['kind']=='reply' for x in d.logs) or mode=='disconnect')
                    finally:
                        d.stop.set();d.thread.join(3)

    def test_new_session_clears_contacts_and_ring_is_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=self.device(tmp)
            try:
                d.record(observation())
                self.assertEqual(len(d.contacts.items),1)
                d.record({'schema':1,'kind':'status','session':'boot2','receiver_id':d.info['id']})
                self.assertEqual(len(d.contacts.items),0)
                for i in range(dashboard.MAX_LOGS+10):
                    d.record({'schema':1,'kind':'reply','session':'boot2','ok':True})
                self.assertEqual(len(d.logs),dashboard.MAX_LOGS)
                with self.assertRaises(OSError):
                    d.record({'schema':1,'kind':'status','receiver_id':'ffffffffffff'})
            finally:d.log_handler.close()

    def test_operation_releases_only_its_port_and_never_overlaps_reader(self):
        active=set()
        lock=threading.Lock()
        class Stream:
            def __init__(self,port):self.port=port
            def __enter__(self):
                with lock:
                    if self.port in active:raise AssertionError('duplicate reader')
                    active.add(self.port)
                return self
            def __exit__(self,*args):
                with lock:active.remove(self.port)
            def write(self,value):pass
            def read(self,size):
                time.sleep(.01)
                return (json.dumps({'schema':1,'kind':'status','session':'boot','receiver_id':self.port.split('/')[-1],'radio_ready':True})+'\n').encode()
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(bench,'serial_modules',return_value=(SimpleNamespace(SerialException=OSError),None)), \
             patch.object(bench,'connect',side_effect=Stream):
            a=self.device(tmp,start=True);b=self.device(tmp,'102030405061',start=True)
            try:
                deadline=time.monotonic()+2
                while len(active)!=2 and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(len(active),2)
                entered=threading.Event();release=threading.Event()
                def work():
                    with lock:
                        self.assertNotIn(a.info['port'],active)
                        self.assertIn(b.info['port'],active)
                    entered.set();release.wait(2)
                    return lambda status:True
                a.begin('test',work)
                self.assertTrue(entered.wait(2))
                with self.assertRaises(RuntimeError):a.begin('other',work)
                release.set()
                deadline=time.monotonic()+3
                while a.operation_lock.locked() and time.monotonic()<deadline:time.sleep(.02)
                self.assertEqual(a.job['state'],'complete')
            finally:
                a.stop.set();b.stop.set();a.thread.join(3);b.thread.join(3)


class HTTPTests(unittest.TestCase):
    def test_static_api_and_mutation_origin_guards(self):
        called=[]
        identify_status={'identifying':True,'session':'boot'}
        device=SimpleNamespace(configure=lambda body: called.append(body) or 'job1',
            configure_uart=lambda body: called.append(body) or 'uart1',
            flash_guard=lambda board,fingerprint:called.append(('guard-flash',board,fingerprint)) or 'guard1',
            check_guard=lambda board:called.append(('guard-check',board)) or 'guard2',
            test_guard_uart=lambda body:called.append(('guard-uart-test',body)) or 'guard3',
            provision=lambda firmware,body:called.append(('provision',body)) or 'role1',
            identify=lambda:called.append('identify') or identify_status)
        manager=SimpleNamespace(token='test-token',snapshot=lambda:{'devices':[]},
            firmware=SimpleNamespace(info=lambda:{'available':False}),device=lambda key:device)
        base=server.handler_for(manager)
        def request(method,path,body=None,headers=None):
            payload=b'' if body is None else json.dumps(body).encode()
            fields={'Host':'127.0.0.1:8766','Connection':'close'}
            if payload: fields.update({'Content-Type':'application/json','Content-Length':str(len(payload))})
            fields.update(headers or {})
            raw=(f'{method} {path} HTTP/1.1\r\n'+''.join(f'{k}: {v}\r\n' for k,v in fields.items())+'\r\n').encode()+payload
            output=io.BytesIO()
            class Handler(base):
                def setup(self):self.rfile=io.BytesIO(raw);self.wfile=output
                def finish(self):pass
            Handler(None,('127.0.0.1',1234),SimpleNamespace(server_port=8766))
            response=output.getvalue()
            return int(response.split(b' ')[1]),response.split(b'\r\n\r\n',1)[1]
        self.assertEqual(request('GET','/')[0],200)
        self.assertEqual(request('GET','/README.md')[0],200)
        code,body=request('GET','/api/state')
        self.assertEqual(code,200);self.assertEqual(json.loads(body)['token'],'test-token')
        self.assertEqual(request('GET','/../tools/server.py')[0],404)
        self.assertEqual(request('GET','/api/state',headers={'Host':'evil.example:8766'})[0],403)
        self.assertEqual(request('POST','/api/devices/abc/configure',{'hello':'A'})[0],403)
        self.assertEqual(request('POST','/api/devices/abc/configure',{'hello':'A'},
            {'X-DroneID-Token':'test-token','Origin':'https://evil.example'})[0],403)
        self.assertEqual(called,[])
        self.assertEqual(request('POST','/api/devices/abc/configure',{'hello':'A'},
            {'X-DroneID-Token':'test-token','Origin':'http://127.0.0.1:8766'})[0],202)
        self.assertEqual(called,[{'hello':'A'}])
        self.assertEqual(request('POST','/api/devices/abc/uart',{'enabled':True},
            {'X-DroneID-Token':'test-token','Origin':'http://127.0.0.1:8766'})[0],202)
        self.assertEqual(called[-1],{'enabled':True})
        code,body=request('POST','/api/devices/abc/identify',{},
            {'X-DroneID-Token':'test-token','Origin':'http://127.0.0.1:8766'})
        self.assertEqual(code,200)
        self.assertEqual(json.loads(body),{'ok':True,'status':identify_status})
        self.assertEqual(called[-1],'identify')
        self.assertEqual(request('GET','/guard/README.md')[0],200)
        for action in ('guard-flash','guard-check','guard-uart-test','provision'):
            payload={'board':'xiao_esp32c6','fingerprint':'reviewed-build','fixture_confirmed':True}
            before=len(called)
            self.assertEqual(request('POST','/api/devices/abc/'+action,payload)[0],403)
            self.assertEqual(len(called),before)
            self.assertEqual(request('POST','/api/devices/abc/'+action,payload,
                {'X-DroneID-Token':'test-token','Origin':'http://127.0.0.1:8766'})[0],202)
            self.assertEqual(called[-1][0],action)


if __name__=='__main__':unittest.main()
