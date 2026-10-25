"""Bounded schema-1 USB input validation shared by CLI and dashboard.

Unknown fields fail closed. Optional fields allow older firmware without newly
added telemetry/identify features; present fields are always fully validated.
This validates shape and range, not the truth or authenticity of device claims.
"""
import copy
import json
import math
import re

MAX_LINE = 4096
MAX_DEPTH = 6
MAX_NODES = 256
U32 = ('int', 0, 2**32-1)
U64 = ('int', 0, 2**64-1)
BOOL = ('bool',)


def integer(lo, hi): return ('int', lo, hi)
def number(lo, hi): return ('number', lo, hi)
def string(size, pattern=None): return ('str', size, pattern)
def enum(*values): return ('enum', values)
def nullable(spec): return ('nullable', spec)
def object_of(fields, required=None): return ('object', fields, set(fields) if required is None else set(required))
def counters(names): return {name: U32 for name in names.split()}


def check(value, spec, path):
    kind = spec[0]
    valid = False
    if kind == 'nullable':
        if value is not None: check(value, spec[1], path)
        return
    if kind in ('int', 'number'):
        valid = (type(value) is int if kind == 'int' else type(value) in (int, float))
        valid = valid and spec[1] <= value <= spec[2]
    elif kind == 'bool': valid = type(value) is bool
    elif kind == 'str':
        valid = type(value) is str and len(value) <= spec[1]
        valid = valid and (spec[2] is None or re.fullmatch(spec[2], value) is not None)
    elif kind == 'enum':
        valid = any(type(value) is type(x) and value == x for x in spec[1])
    elif kind == 'list':
        valid = type(value) is list and len(value) <= spec[2]
        if valid:
            for item in value: check(item, spec[1], path)
    elif kind == 'object':
        valid = type(value) is dict and spec[2] <= value.keys() <= spec[1].keys()
        if valid:
            for key, item in value.items(): check(item, spec[1][key], path+'.'+key)
    if not valid:
        # Paths contain only locally defined schema keys, never attacker values.
        raise ValueError('Invalid USB field: '+path)


def bounded_tree(value):
    remaining = MAX_NODES
    def visit(item, depth):
        nonlocal remaining
        remaining -= 1
        if depth > MAX_DEPTH or remaining < 0:
            raise ValueError('USB record exceeds structural limits')
        if type(item) is dict:
            for key, child in item.items():
                if type(key) is not str: raise ValueError('Invalid USB object key')
                visit(key, depth+1)
                visit(child, depth+1)
        elif type(item) is list:
            for child in item: visit(child, depth+1)
        elif type(item) is str:
            if len(item) > 256 or any(0xd800 <= ord(c) <= 0xdfff for c in item):
                raise ValueError('Invalid USB string')
        elif type(item) is int:
            if not -(2**63) <= item < 2**64: raise ValueError('USB integer out of range')
        elif type(item) is float:
            if not math.isfinite(item): raise ValueError('Non-finite USB number')
        elif item is not None and type(item) is not bool:
            raise ValueError('Invalid USB value type')
    visit(value, 0)


COMMON = {'schema': enum(1), 'kind': enum('observation','status','reply'),
          'receiver_id': string(12, r'[0-9a-f]{12}'),
          'session': string(64, r'[A-Za-z0-9_-]{1,64}')}
TOKEN = string(32, r'[A-Za-z0-9_-]{0,32}')
TRANSPORT = enum('ble_legacy','wifi_beacon','wifi_nan')
POSITION = object_of({'lat_e7':integer(-900000000,900000000), 'lon_e7':integer(-1800000000,1800000000),
                      'altitude_msl_mm':integer(-2**31,2**31-1), 'relative_altitude_mm':integer(-2**31,2**31-1)})
UART_FIELDS = {'configuration_version':enum(1), 'enabled':BOOL, 'ready':BOOL,
    'baud':enum(57600,115200,230400,460800), 'tx_pin':enum(0,1,25,7,23,24,11,12,8,9,10),
    'rx_pin':enum(0,1,25,7,23,24,11,12,8,9,10), 'system_id':integer(1,255),
    'component_id':integer(1,255), 'fc_component_id':integer(1,255),
    'target_system':integer(0,255), 'target_component':integer(0,255), 'max_rate':integer(1,50),
    'connected':BOOL, 'heartbeat_age_ms':integer(-1,2**63-1), 'position_age_ms':integer(-1,2**63-1),
    'gps_age_ms':integer(-1,2**63-1), 'position':nullable(POSITION),
    'gps':nullable(object_of({'fix_type':integer(0,8),'satellites':integer(0,255)})),
    **counters('tx_frames rx_frames queue_drops expired parse_errors signed_ignored errors')}
SCOUT_FIELDS = {**COMMON, 'request_id':TOKEN, 'role':enum('rx','tx','both'),
    'firmware':string(32,r'[A-Za-z0-9.+_-]{1,32}'), 'configuration_version':enum(1),
    'broadcast_id':string(19,r'[A-Za-z0-9._-]{1,19}'), 'hello_hex':string(46,r'(?:[0-9a-f]{2}){0,23}'),
    'identifying':BOOL, 'identify_supported':BOOL, 'tx_transport':enum('ble_legacy','wifi_beacon'),
    'rx_transports':('list',TRANSPORT,3), 'rx_schedule':enum('disabled','time_sliced'),
    'rx_phase':enum('idle','ble','wifi_6','wifi_149'), 'radio_ready':BOOL, 'bench_transmitter':BOOL,
    'uptime_us':U64, 'uart':nullable(object_of(UART_FIELDS)),
    **counters('free_heap_bytes advertisements_seen rid_observations malformed_advertisements rx_queue_dropped '
               'usb_records_dropped tx_advertisement_sets radio_errors ble_observations wifi_observations '
               'wifi_frames_seen wifi_tx_frames scan_cycles ble_listen_ms wifi_6_listen_ms wifi_149_listen_ms')}
SCOUT_REQUIRED = {*COMMON, 'request_id','role','firmware','configuration_version','broadcast_id','hello_hex',
                  'tx_transport','radio_ready','uptime_us'}
GUARD_COUNTERS = counters('input_bytes dropped_bytes rejected framing_errors uart_errors accepted replaced expired output_frames output_bytes')
GUARD_FIELDS = {**COMMON,'device_type':enum('uart_guard'),'firmware_project':enum('droneid_guard'),
    'firmware':SCOUT_FIELDS['firmware'], 'board_id':enum('xiao_esp32c5','xiao_esp32c6','xiao_esp32s3'),
    'chip':enum('esp32c5','esp32c6','esp32s3'), 'app_sha256':string(64,r'[0-9a-f]{64}'),
    'ready':BOOL,'policy_selftest':BOOL,'radios_enabled':BOOL,'uptime_ms':U64,
    **{key:integer(1,255) for key in ('system_id','component_id','sensor_component','fc_component','target_system','target_component')},
    **{side:object_of(GUARD_COUNTERS, 'input_bytes dropped_bytes rejected framing_errors uart_errors'.split()) for side in ('sensor','fc')},
    'encode_errors':U32}
GUARD_REQUIRED = set(GUARD_FIELDS)-{'encode_errors'}

ALT = nullable(number(-1000,31767.5))
DATA = {
    'basic_id':object_of({'id_type':integer(0,15),'ua_type':integer(0,15),'uas_id_hex':string(40,r'[0-9a-f]{40}'),
                          'uas_id_text':nullable(string(20))}),
    'self_id':object_of({'description_type':integer(0,255),'description':string(23)}),
    'operator_id':object_of({'operator_id_type':integer(0,255),'operator_id':string(20)}),
    'authentication':object_of({'auth_type':integer(0,15),'page':integer(0,15)}),
    'location':object_of({'status':integer(0,15),'height_reference':integer(0,1),
        'latitude_deg':nullable(number(-90,90)), 'longitude_deg':nullable(number(-180,180)),
        'altitude_geodetic_m':ALT, 'altitude_barometric_m':ALT, 'height_m':ALT,
        'direction_deg':nullable(number(0,360)), 'horizontal_speed_mps':nullable(number(0,254.25)),
        'vertical_speed_mps':nullable(number(-62,62)), 'seconds_after_hour':nullable(number(0,3600)),
        **{key:integer(0,15) for key in ('horizontal_accuracy_code','vertical_accuracy_code','barometric_accuracy_code',
                                       'speed_accuracy_code','time_accuracy_code')}}),
    'system':object_of({'operator_location_type':integer(0,3),'classification_type':integer(0,7),
        'operator_latitude_deg':nullable(number(-90,90)),'operator_longitude_deg':nullable(number(-180,180)),
        'operator_altitude_geodetic_m':ALT,'timestamp_since_2019_s':nullable(number(0,2**32-1)),
        'area_count':integer(0,65535),'area_radius_m':integer(0,2550),'category_eu':integer(0,15),
        'class_eu':integer(0,15),'area_ceiling_m':ALT,'area_floor_m':ALT})}
OBSERVATION = {**COMMON,'seq':U32,'rx_us':U64,'reported_us':U64,'transport':TRANSPORT,
    'phy':nullable(enum('1M')),'channel':nullable(integer(1,233)),'frequency_mhz':nullable(integer(2400,6200)),
    'pack_index':integer(0,8),'pack_size':integer(1,9),'rssi_dbm':nullable(integer(-127,126)),
    'source_address':string(17,r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}'),
    'source_address_type':nullable(integer(0,3)),'message_counter':integer(0,255),'protocol_version':integer(0,15),
    'message_type':enum(*DATA,'unsupported'),'authentication':enum('unverified'),
    'raw_message_hex':string(50,r'[0-9a-f]{50}'),'decode_status':enum('ok','invalid','unsupported')}


def validate(record):
    """Reject before consumers can store, serialize or act on any field."""
    bounded_tree(record)
    if type(record) is not dict: raise ValueError('USB record must be an object')
    for key, spec in COMMON.items(): check(record.get(key), spec, key)
    kind = record['kind']
    if kind == 'reply':
        check(record, object_of({**COMMON,'request_id':TOKEN,'ok':BOOL,'message':string(160,r'[ -~]*')}), 'reply')
    elif kind == 'status':
        guard = 'device_type' in record or 'firmware_project' in record
        check(record, object_of(GUARD_FIELDS,GUARD_REQUIRED) if guard else object_of(SCOUT_FIELDS,SCOUT_REQUIRED), 'status')
        if guard and record['board_id'] != 'xiao_'+record['chip']:
            raise ValueError('Inconsistent guard model')
        if not guard:
            if any(not 32 <= b <= 126 for b in bytes.fromhex(record['hello_hex'])):
                raise ValueError('Invalid Scout hello bytes')
            uart = record.get('uart')
            if uart and (uart['tx_pin']==uart['rx_pin'] or uart['component_id']==uart['fc_component_id']):
                raise ValueError('Conflicting UART settings')
    else:
        check({k:v for k,v in record.items() if k!='data'}, object_of(OBSERVATION), 'observation')
        if 'data' not in record: raise ValueError('Missing observation data')
        if record['rx_us'] > record['reported_us'] or record['pack_index'] >= record['pack_size']:
            raise ValueError('Invalid observation ordering')
        raw = bytes.fromhex(record['raw_message_hex'])
        types = ('basic_id','location','authentication','self_id','system','operator_id')
        expected = types[raw[0] >> 4] if raw[0] >> 4 < len(types) else 'unsupported'
        if record['message_type'] != expected or record['protocol_version'] != raw[0] & 15:
            raise ValueError('Observation type differs from raw message')
        if record['decode_status']=='ok':
            if record['message_type'] not in DATA: raise ValueError('Unsupported decoded message')
            check(record['data'], DATA[record['message_type']], 'observation.data')
        elif record['data'] is not None:
            raise ValueError('Undecoded observation must have null data')
        if record['transport']=='ble_legacy':
            if record['phy']!='1M' or record['channel'] is not None or record['frequency_mhz'] is not None or record['source_address_type'] is None:
                raise ValueError('Invalid BLE metadata')
        elif record['phy'] is not None or record['source_address_type'] is not None:
            raise ValueError('Invalid Wi-Fi metadata')
    if len(json.dumps(record, ensure_ascii=True, allow_nan=False, separators=(',',':'))) > MAX_LINE:
        raise ValueError('USB record exceeds encoded size limit')


def snapshot(record):
    validate(record)
    return copy.deepcopy(record)
