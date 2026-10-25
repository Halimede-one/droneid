"""Trusted local guard profiles, build checks and read-only USB diagnostics."""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CATALOG = json.loads((ROOT/'guard/boards.json').read_text())
PROFILES = {p['id']: p for p in CATALOG if p['supported']}

def profile(board):
    if not isinstance(board, str) or board not in PROFILES:
        raise ValueError('Choose a supported XIAO guard model.')
    return PROFILES[board]

class GuardFirmware:
    def __init__(self, board, build_dir=None):
        self.profile = profile(board)
        self.build_dir = Path(build_dir or ROOT/'guard/build'/board).resolve()

    def bundle(self):
        root, p = self.build_dir, self.profile
        desc = json.loads((root/'project_description.json').read_text())
        manifest = json.loads((root/'flasher_args.json').read_text())
        if desc.get('project_name') != 'droneid_guard' or desc.get('target') != p['chip'] or manifest.get('extra_esptool_args', {}).get('chip') != p['chip']:
            raise ValueError('Guard project/target does not match selected model.')
        if desc.get('project_version') != '0.2.1':
            raise ValueError('Build guard 0.2.1; older builds lack the factory-MAC buffer fix.')
        if manifest.get('flash_settings') != {'flash_mode':'dio','flash_size':p['flash_size'],'flash_freq':'80m'}:
            raise ValueError('Guard flash mode/size/frequency does not match the profile.')
        components = set(desc['build_components'])
        if components & {'esp_wifi','bt','lwip','esp_netif','esp_phy','esp_coex'}:
            raise ValueError('Guard build contains radio/network components.')
        if not {'uart_guard','esp_driver_uart','esp_driver_usb_serial_jtag'} <= components:
            raise ValueError('Guard or serial components missing.')
        config_path = Path(desc['config_file']).resolve()
        # Config is local trusted build metadata, not a client-supplied path.
        config = config_path.read_text().splitlines()
        for required in ('CONFIG_COMPILER_STACK_CHECK_MODE_ALL=y','CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y',
                         'CONFIG_ESPTOOLPY_FLASHSIZE="'+p['flash_size']+'"'):
            if required not in config:
                raise ValueError('Required guard configuration missing: '+required)
        files = manifest.get('flash_files', {})
        limits = {p['boot_offset']:0x6000, '0x8000':0x1000, '0x10000':0x200000}
        if set(files) != set(limits):
            raise ValueError('Unexpected guard flash layout.')
        digest = hashlib.sha256(json.dumps(p, sort_keys=True).encode())
        images = []
        for offset, name in sorted(files.items(), key=lambda pair:int(pair[0],16)):
            path = (root/name).resolve()
            if not path.is_relative_to(root):
                raise ValueError('Guard image outside build directory.')
            data = path.read_bytes()
            if not 0 < len(data) <= limits[offset]:
                raise ValueError('Guard image does not fit partition.')
            digest.update(offset.encode()+b'\0'+data)
            images.append((offset,path.name,data))
        app = next(data for offset, _, data in images if offset == '0x10000')
        # ESP image header(24) + segment header(8) + esp_app_desc; ELF SHA at 144.
        if len(app) < 208 or app[0] != 0xe9 or app[32:36] != b'\x32\x54\xcd\xab':
            raise ValueError('Guard application descriptor missing.')
        if app[48:80].split(b'\0',1)[0] != desc['project_version'].encode() or app[80:112].split(b'\0',1)[0] != b'droneid_guard':
            raise ValueError('Guard application project/version differs from build metadata.')
        app_sha = app[176:208].hex()
        if app_sha == '0'*64:
            raise ValueError('Guard application has no build hash.')
        return {'board_id':p['id'], 'version':desc['project_version'], 'chip':p['chip'],
                'flash_size':p['flash_size'], 'fingerprint':digest.hexdigest(), 'app_sha256':app_sha,
                'size':sum(len(i[2]) for i in images), 'images':images}

    def info(self):
        try:
            return {k:v for k,v in self.bundle().items() if k!='images'} | {'available':True}
        except (OSError, ValueError, KeyError, TypeError) as exc:
            return {'available':False, 'error':str(exc)}

def check_status(status, bundle):
    """Strict allowlist for a fresh diagnostic sample; not firmware attestation."""
    expected = {'device_type':'uart_guard', 'firmware_project':'droneid_guard',
                'board_id':bundle['board_id'], 'chip':bundle['chip'], 'firmware':bundle['version'],
                'app_sha256':bundle['app_sha256'], 'ready':True, 'radios_enabled':False, 'policy_selftest':True}
    for key, value in expected.items():
        if type(status.get(key)) is not type(value) or status[key] != value:
            raise ValueError('Guard diagnostic mismatch: '+key)
    if not isinstance(status.get('session'),str) or not re.fullmatch('[0-9a-f]{16}',status['session']):
        raise ValueError('Guard boot session missing.')
    if type(status.get('uptime_ms')) is not int or not 0 <= status['uptime_ms'] < 2**64:
        raise ValueError('Guard uptime invalid.')
    for key in ('system_id','component_id','sensor_component','fc_component','target_system','target_component'):
        if type(status.get(key)) is not int or not 1 <= status[key] <= 255 or status[key] in (253,254):
            raise ValueError('Guard policy ID invalid: '+key)
    for side in ('sensor','fc'):
        fields = status.get(side)
        if not isinstance(fields,dict):
            raise ValueError('Guard counters missing.')
        for key in ('input_bytes','dropped_bytes','rejected','framing_errors','uart_errors'):
            if type(fields.get(key)) is not int or not 0 <= fields[key] <= 2**32-1:
                raise ValueError('Guard counter invalid: '+side+'.'+key)
    return True

def catalog():
    return [dict(p, build=GuardFirmware(p['id']).info() if p['supported'] else None) for p in CATALOG]
