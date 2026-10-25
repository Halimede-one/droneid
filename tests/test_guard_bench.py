import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
import bench
import dashboard
import guard_bench as gb
import guard_fixture as gf


def build(root, board='xiao_esp32c5'):
    p=gb.profile(board)
    config=root/'sdkconfig'
    config.write_text('CONFIG_COMPILER_STACK_CHECK_MODE_ALL=y\nCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y\nCONFIG_ESPTOOLPY_FLASHSIZE="'+p['flash_size']+'"\n')
    desc={'project_name':'droneid_guard','target':p['chip'],'project_version':'0.2.1','config_file':str(config),
          'build_components':['uart_guard','esp_driver_uart','esp_driver_usb_serial_jtag']}
    (root/'project_description.json').write_text(json.dumps(desc))
    app=bytearray(256);app[0]=0xe9;app[32:36]=b'\x32\x54\xcd\xab';app[176:208]=bytes(range(32))
    app[48:53]=b'0.2.1';app[80:93]=b'droneid_guard'
    (root/'app.bin').write_bytes(app)
    (root/'boot.bin').write_bytes(b'boot');(root/'partition.bin').write_bytes(b'part')
    manifest={'extra_esptool_args':{'chip':p['chip']},'flash_settings':{'flash_mode':'dio','flash_size':p['flash_size'],'flash_freq':'80m'},'flash_files':{p['boot_offset']:'boot.bin','0x8000':'partition.bin','0x10000':'app.bin'}}
    (root/'flasher_args.json').write_text(json.dumps(manifest))
    return gb.GuardFirmware(board,root)


def status(bundle,uptime=100):
    return {'schema':1,'kind':'status','receiver_id':'102030405060','device_type':'uart_guard','firmware_project':'droneid_guard',
        'board_id':bundle['board_id'],'chip':bundle['chip'],'firmware':bundle['version'],'app_sha256':bundle['app_sha256'],
        'ready':True,'radios_enabled':False,'policy_selftest':True,'session':'0123456789abcdef','uptime_ms':uptime,
        'system_id':1,'component_id':243,'sensor_component':242,'fc_component':1,'target_system':255,'target_component':190,
        'sensor':dict(input_bytes=0,dropped_bytes=0,rejected=0,framing_errors=0,uart_errors=0),
        'fc':dict(input_bytes=0,dropped_bytes=0,rejected=0,framing_errors=0,uart_errors=0)}


class GuardBuildTests(unittest.TestCase):
    def test_invalid_guard_identity_preserves_protection_without_accepting_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            info={'id':'3844bebe0c20','serial':'38:44:BE:BE:0C:20','port':'/fake','tty':'/fake'}
            d=dashboard.Device(info,log_dir=tmp,start=False)
            try:
                with self.assertRaisesRegex(OSError,'identity'):
                    d.record({'kind':'status','receiver_id':'3844befeffbe',
                              'device_type':'uart_guard','firmware':'0.2.0','ready':True})
                self.assertEqual(d.snapshot()['device_type'],'uart_guard')
                self.assertIsNone(d.status)
                with self.assertRaises(ValueError):d.flash(None,None)
            finally:d.log_handler.close()

    def test_profiles_layout_immutable_snapshot_and_descriptor_hash(self):
        for board in gb.PROFILES:
            with self.subTest(board=board),tempfile.TemporaryDirectory() as tmp:
                fw=build(Path(tmp),board);bundle=fw.bundle()
                self.assertEqual(bundle['app_sha256'],bytes(range(32)).hex())
                self.assertEqual(bundle['images'][0][0],gb.profile(board)['boot_offset'])
                old=bundle['images'][-1][2]
                (Path(tmp)/'app.bin').write_bytes(old+b'x')
                self.assertEqual(bundle['images'][-1][2],old)
                self.assertNotEqual(fw.bundle()['fingerprint'],bundle['fingerprint'])
        for board in ('../esp32c5','xiao_rp2350',None,{},'esp32c3'):
            with self.assertRaises(ValueError):gb.GuardFirmware(board)

    def test_wrong_target_radio_components_config_paths_and_layout_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for field,value in [('project_name','droneid'),('target','esp32c6'),('project_version','0.1.0'),('project_version','0.2.0'),
                                ('build_components',['esp_wifi']),('build_components',['bt','lwip'])]:
                fw=build(root);desc=json.loads((root/'project_description.json').read_text());desc[field]=value
                (root/'project_description.json').write_text(json.dumps(desc));self.assertFalse(fw.info()['available'])
            for name in ('../app.bin','/etc/passwd'):
                fw=build(root);manifest=json.loads((root/'flasher_args.json').read_text());manifest['flash_files']['0x10000']=name
                (root/'flasher_args.json').write_text(json.dumps(manifest));self.assertFalse(fw.info()['available'])
            fw=build(root);(root/'sdkconfig').write_text('');self.assertFalse(fw.info()['available'])
            fw=build(root);(root/'app.bin').write_bytes(b'no app descriptor');self.assertFalse(fw.info()['available'])
            fw=build(root);app=bytearray((root/'app.bin').read_bytes());app[80:93]=b'droneid'+b'\0'*6
            (root/'app.bin').write_bytes(app);self.assertFalse(fw.info()['available'])
            fw=build(root);manifest=json.loads((root/'flasher_args.json').read_text());manifest['flash_settings']['flash_freq']='40m'
            (root/'flasher_args.json').write_text(json.dumps(manifest));self.assertFalse(fw.info()['available'])

    def test_fresh_diagnostics_match_build_and_require_strict_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle=build(Path(tmp)).bundle();good=status(bundle)
            self.assertTrue(gb.check_status(good,bundle))
            for key,value in [('ready',1),('radios_enabled',0),('policy_selftest',False),('board_id','xiao_esp32c6'),
                              ('app_sha256','0'*64),('session','guard'),('uptime_ms',float('inf')),('uptime_ms',True),('system_id',253)]:
                with self.subTest(key=key),self.assertRaises(ValueError):gb.check_status(good|{key:value},bundle)
            bad=copy.deepcopy(good);bad['fc']['uart_errors']=1e309
            with self.assertRaises(ValueError):gb.check_status(bad,bundle)
            decoder=bench.LineDecoder()
            self.assertEqual(decoder.feed(b'{"schema":1,"kind":"status","uptime_ms":1e309}\n'),[])
            self.assertEqual(len(decoder.feed((json.dumps(good)+'\n').encode())),1)

    def test_usb_check_requires_two_samples_and_rejects_reboot(self):
        with tempfile.TemporaryDirectory() as tmp:
            fw=build(Path(tmp));bundle=fw.bundle()
            info={'id':'102030405060','serial':'102030405060','port':'/fake','tty':'/fake'}
            d=dashboard.Device(info,log_dir=tmp,start=False);d.guard_seen=True
            d.job={'lines':[]}
            try:
                with patch.object(gb,'GuardFirmware',return_value=fw),patch.object(d,'begin',side_effect=lambda kind,work,**kw:work()):
                    verify=d.check_guard('xiao_esp32c5')
                    self.assertFalse(verify(status(bundle)))
                    self.assertFalse(verify(status(bundle)))
                    self.assertTrue(verify(status(bundle,200)))
                    with self.assertRaises(ValueError):verify(status(bundle,300)|{'session':'f'*16})
                    with self.assertRaises(ValueError):d.flash_guard('xiao_esp32c5','stale-fingerprint')
                with self.assertRaises(ValueError):d.flash(None,None)
                with self.assertRaises(ValueError):d.test_guard_uart({'fixture_confirmed':False})
            finally:d.log_handler.close()

    def test_guard_post_operation_accepts_ready_without_radio_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            info={'id':'102030405060','serial':'102030405060','port':'/fake','tty':'/fake'}
            d=dashboard.Device(info,log_dir=tmp,start=False)
            try:
                def work():return lambda s:s['policy_selftest'] is True
                d.begin('guard-check',work,guard=True)
                deadline=time.monotonic()+2
                while d.operation_lock.locked() and time.monotonic()<deadline:
                    d.record({'schema':1,'kind':'status','receiver_id':info['id'],'device_type':'uart_guard','ready':True,'policy_selftest':True})
                    time.sleep(.02)
                self.assertEqual(d.job['state'],'complete')
            finally:d.stop.set();d.log_handler.close()

    def test_flash_uses_selected_chip_layout_usb_identity_and_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            fw=build(Path(tmp),'xiao_esp32s3');bundle=fw.bundle()
            info={'id':'102030405060','serial':'102030405060','port':'/stable/selected','tty':'/fake'}
            d=dashboard.Device(info,locator=lambda:{info['id']:info},log_dir=tmp,start=False)
            d.job={'lines':[]};commands=[]
            def popen(command,**kwargs):
                commands.append(command)
                self.assertEqual(command[command.index('--chip')+1],'esp32s3')
                self.assertEqual(command[command.index('--port')+1],info['port'])
                self.assertEqual(command[command.index('--flash-size')+1],'8MB')
                for offset,_,data in bundle['images']:
                    self.assertEqual(Path(command[command.index(offset)+1]).read_bytes(),data)
                return SimpleNamespace(stdout=io.StringIO('verified\n'),wait=lambda:0,kill=lambda:None)
            try:
                with patch.object(gb,'GuardFirmware',return_value=fw),patch.object(d,'begin',side_effect=lambda kind,work,**kw:work()),patch.object(dashboard.subprocess,'Popen',side_effect=popen):
                    verify=d.flash_guard('xiao_esp32s3',bundle['fingerprint'])
                    self.assertTrue(verify(status(bundle)))
                    self.assertTrue(d.guard_seen)
                    self.assertEqual(len(commands),1)
            finally:d.log_handler.close()


class DeviceRoleTests(unittest.TestCase):
    def device(self,directory):
        info={'id':'102030405060','serial':'102030405060','port':'/selected','tty':'/selected'}
        d=dashboard.Device(info,locator=lambda:{info['id']:info},log_dir=directory,start=False)
        d.guard_seen=True;d.status={'board_id':'xiao_esp32c5','device_type':'uart_guard'}
        d.job={'lines':[]}
        return d

    def test_provision_requires_explicit_supported_role_and_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=self.device(tmp)
            try:
                with patch.object(d,'flash') as flash,patch.object(d,'flash_guard') as guard:
                    for values in ({},{'role':'both'},{'role':'radio'},{'role':None},{'role':'scout','board':'xiao_esp32c6'}):
                        with self.assertRaises(ValueError):d.provision(None,values)
                    flash.assert_not_called();guard.assert_not_called()
                    d.provision(None,{'role':'guard','board':'xiao_esp32s3','fingerprint':'guard-build'})
                    guard.assert_called_once_with('xiao_esp32s3','guard-build')
                    d.provision('scout-bundle',{'role':'scout','board':'xiao_esp32c5','fingerprint':'scout-build'})
                    flash.assert_called_once_with('scout-bundle','scout-build',replace_guard=True)
                    d.status['board_id']='xiao_esp32c6'
                    with self.assertRaises(ValueError):d.provision(None,{'role':'scout','board':'xiao_esp32c5'})
            finally:d.log_handler.close()

    def test_guard_to_scout_requires_verified_post_flash_status(self):
        fw=SimpleNamespace(bundle=lambda:{'version':'0.5.2','fingerprint':'scout-build','images':[('0x10000','app.bin',b'app')]})
        with tempfile.TemporaryDirectory() as tmp:
            d=self.device(tmp)
            try:
                with self.assertRaises(ValueError):d.flash(fw,'scout-build')
                with patch.object(d,'begin',side_effect=lambda kind,work:work()),patch.object(dashboard.subprocess,'Popen',return_value=SimpleNamespace(stdout=io.StringIO('flashed'),wait=lambda:0,kill=lambda:None)):
                    verify=d.provision(fw,{'role':'scout','board':'xiao_esp32c5','fingerprint':'scout-build'})
                    self.assertTrue(d.guard_seen)
                    good={'firmware':'0.5.2','radio_ready':True,'configuration_version':1}
                    for bad in (good|{'firmware':'0.5.1'},good|{'device_type':'uart_guard'},good|{'radio_ready':False},good|{'configuration_version':0}):
                        self.assertFalse(verify(bad));self.assertTrue(d.guard_seen)
                    self.assertTrue(verify(good));self.assertFalse(d.guard_seen)
            finally:d.log_handler.close()

    def test_failed_role_conversion_keeps_guard_protection(self):
        fw=SimpleNamespace(bundle=lambda:{'version':'0.5.2','fingerprint':'scout-build','images':[]})
        with tempfile.TemporaryDirectory() as tmp:
            d=self.device(tmp)
            try:
                with patch.object(d,'begin',side_effect=lambda kind,work:work()),patch.object(dashboard.subprocess,'Popen',return_value=SimpleNamespace(stdout=io.StringIO('failed'),wait=lambda:1,kill=lambda:None)):
                    with self.assertRaises(RuntimeError):d.provision(fw,{'role':'scout','board':'xiao_esp32c5','fingerprint':'scout-build'})
                    self.assertTrue(d.guard_seen)
            finally:d.log_handler.close()


class GuardFixtureTests(unittest.TestCase):
    def test_reference_uart_fixture_success_and_leak_or_missing_report_failure(self):
        try:
            from pymavlink.dialects.v20 import common as mav
        except ImportError:self.skipTest('Optional pymavlink unavailable')
        import base64
        for failure in (None,'leak','missing'):
            with self.subTest(failure=failure):
                tick=[0.0]
                def sleep(seconds):tick[0]+=seconds
                class Port:
                    def __init__(self,side):self.side=side;self.queue=bytearray();self.parser=mav.MAVLink(None);self.parser.robust_parsing=True;self.timeout=0
                    def __enter__(self):return self
                    def __exit__(self,*args):pass
                    def reset_input_buffer(self):self.queue.clear()
                    def read(self,n):data=bytes(self.queue[:n]);del self.queue[:n];return data
                    def write(self,data):
                        for msg in self.parser.parse_buffer(data) or []:
                            if msg.get_type()=='BAD_DATA':continue
                            if self.side=='fc' and msg.get_type()=='HEARTBEAT':ports['sensor'].queue.extend(msg.get_msgbuf())
                            if self.side=='sensor' and msg.get_type()=='TUNNEL' and failure!='missing':
                                raw=b'DRG2'+base64.b64encode(bytes(msg.payload[:78]))
                                out=mav.MAVLink_tunnel_message(255,190,0,len(raw),raw+b'\0'*(128-len(raw)))
                                tx=mav.MAVLink(None,srcSystem=1,srcComponent=243)
                                for seq in range(256):
                                    tx.seq=seq;wire=out.pack(tx)
                                    if not any(b in (0xfd,0xfe) for b in wire[1:]):break
                                ports['fc'].queue.extend(wire)
                            if self.side=='sensor' and msg.get_type()=='PARAM_REQUEST_LIST' and failure=='leak':ports['fc'].queue.extend(msg.get_msgbuf())
                ports={side:Port(side) for side in ('sensor','fc')}
                with patch.object(gf,'adapters',return_value={x:{'port':x} for x in ports}),patch.object(bench,'connect',side_effect=lambda path:ports[path]),patch.object(gf.time,'monotonic',side_effect=lambda:tick[0]),patch.object(gf.time,'sleep',side_effect=sleep):
                    if failure:
                        with self.assertRaises(RuntimeError):gf.run('sensor','fc',lambda x:None)
                    else:gf.run('sensor','fc',lambda x:None)
        with self.assertRaises(ValueError):gf.run('same','same',lambda x:None)
