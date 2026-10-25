"""Wire-shaped USB fixtures; keep malformed variants explicit in each test."""

def scout(**changes):
    value = dict(schema=1,kind='status',receiver_id='102030405060',session='boot1',request_id='',
        role='both',firmware='0.5.2',configuration_version=1,broadcast_id='SCOUT-01',hello_hex='',
        tx_transport='wifi_beacon',radio_ready=True,uptime_us=100)
    value.update(changes)
    return value


def reply(**changes):
    value = dict(schema=1,kind='reply',receiver_id='102030405060',session='boot1',request_id='',ok=True,message='ok')
    value.update(changes)
    return value


def guard(**changes):
    counts = dict(input_bytes=0,dropped_bytes=0,rejected=0,framing_errors=0,uart_errors=0)
    value = dict(schema=1,kind='status',receiver_id='102030405060',session='0123456789abcdef',
        device_type='uart_guard',firmware_project='droneid_guard',board_id='xiao_esp32c5',chip='esp32c5',
        firmware='0.2.1',app_sha256='1'*64,ready=True,policy_selftest=True,radios_enabled=False,uptime_ms=100,
        system_id=1,component_id=243,sensor_component=242,fc_component=1,target_system=255,target_component=190,
        sensor=dict(counts),fc=dict(counts))
    value.update(changes)
    return value


def observation(kind='basic_id', **changes):
    raw = b'\x32\x00hello'.ljust(25,b'\0') if kind=='self_id' else b'\x02\x41\xffSCOUT-01'.ljust(25,b'\0')
    data = dict(description_type=0,description='hello') if kind=='self_id' else dict(id_type=4,ua_type=1,
        uas_id_hex=(b'\xffSCOUT-01').ljust(20,b'\0').hex(),uas_id_text=None)
    value = dict(schema=1,kind='observation',receiver_id='102030405060',session='boot1',seq=1,
        source_address='10:20:30:40:50:60',source_address_type=None,transport='wifi_beacon',
        phy=None,channel=6,frequency_mhz=2437,rssi_dbm=-55,pack_index=0,pack_size=1,
        message_counter=1,protocol_version=2,message_type=kind,authentication='unverified',
        raw_message_hex=raw.hex(),decode_status='ok',rx_us=100,reported_us=100,data=data)
    value.update(changes)
    return value
