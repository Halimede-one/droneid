#!/usr/bin/env python3
"""Decode DroneID TUNNEL reports from a serial port or MAVLink UDP downlink."""

import argparse
import base64
import binascii
from datetime import datetime, timezone
import json
import struct
import time

HEADER=struct.Struct('<4sBB6sQIQQ')
TRANSPORTS={0:'ble_legacy',1:'wifi_beacon',2:'wifi_nan'}
HEALTH_FIELDS=('uart_queue_drops','uart_expired','uart_tx_frames','uart_rx_frames',
               'uart_parse_errors','uart_errors','fc_heartbeat_age_ms',
               'radio_errors','rx_queue_dropped','usb_records_dropped','rid_observations')


def decode_payload(payload, length):
    """Pure decoder for the versioned payload. Unrelated tunnels return None."""
    if not 0<=length<=128 or len(payload)<length:
        raise ValueError('Invalid TUNNEL payload length')
    data=bytes(payload[:length])
    guarded = data.startswith(b'DRG2')
    if guarded:
        if len(data) not in (108,116):
            raise ValueError('Invalid guarded report size')
        try:
            raw=base64.b64decode(data[4:],validate=True)
        except (ValueError,binascii.Error) as exc:
            raise ValueError('Invalid guarded report encoding') from exc
        if base64.b64encode(raw)!=data[4:] or not raw.startswith(b'DRID'):
            raise ValueError('Invalid guarded report content')
        data=raw
    if not data.startswith(b'DRID'):
        return None
    if len(data)<HEADER.size:
        raise ValueError('Truncated DroneID header')
    _,version,kind,receiver,session,seq,received,reported=HEADER.unpack_from(data)
    if version!=1:raise ValueError('Unsupported DroneID payload version')
    record={'schema':1,'kind':'observation' if kind==1 else 'uart_health',
            'receiver_id':receiver.hex(),'session':f'{session:016x}','seq':seq,
            'rx_us':received,'reported_us':reported}
    if guarded:
        # This is an encoding claim, not proof of a trusted physical guard or
        # authenticated reporter. An untrusted sender can generate it too.
        record['wire_encoding']='drg2_base64'
    if received>reported:raise ValueError('Receive time is later than reporting time')
    if kind==1:
        if len(data)!=78:raise ValueError('Invalid observation size')
        transport,rssi,address,address_type,channel,counter,index,size,raw=struct.unpack_from('<Bb6sBBBBB25s',data,40)
        if transport not in TRANSPORTS or not 1<=size<=9 or index>=size:
            raise ValueError('Invalid observation metadata')
        names={0:'basic_id',1:'location',2:'authentication',3:'self_id',4:'system',5:'operator_id'}
        record.update(transport=TRANSPORTS[transport],rssi_dbm=rssi if rssi!=-128 else None,
            source_address=':'.join(f'{b:02x}' for b in address),
            source_address_type=address_type if transport==0 else None,
            channel=channel or None,message_counter=counter,pack_index=index,pack_size=size,
            raw_message_hex=raw.hex(),message_type=names.get(raw[0]>>4,'unknown'),
            protocol_version=raw[0]&15,authentication='unverified')
        # The raw message is authoritative. Only bench identity/hello convenience
        # fields are decoded here; use the pinned ODID codec for full aircraft data.
        if raw[0]&15==2 and raw[0]>>4==0:
            value=raw[2:22];id_type=raw[1]>>4
            record['uas_id_hex']=value.hex()
            label=value[1:] if id_type==4 and value[0]==255 else value if id_type in (1,2) else None
            if label is not None:
                label=label.split(b'\0',1)[0]
                if all(32<=b<=126 for b in label):record['broadcast_id']=label.decode('ascii')
        elif raw[0]&15==2 and raw[0]>>4==3:
            record['hello']=raw[2:25].split(b'\0',1)[0].decode('utf-8',errors='replace')
    elif kind==2:
        if len(data)!=84:raise ValueError('Invalid health size')
        record.update(zip(HEALTH_FIELDS,struct.unpack_from('<11I',data,40)))
        if record['fc_heartbeat_age_ms']==0xffffffff:record['fc_heartbeat_age_ms']=None
    else:raise ValueError('Unknown DroneID payload kind')
    return record


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--connect',required=True,help='/dev/ttyUSB0 or udpin:127.0.0.1:14550')
    parser.add_argument('--baud',type=int,default=115200)
    parser.add_argument('--system-id',type=int,default=255)
    parser.add_argument('--component-id',type=int,default=190)
    parser.add_argument('--passive',action='store_true',help='Do not send the GCS heartbeat used to learn routing')
    args=parser.parse_args()
    if not 1<=args.system_id<=255 or not 1<=args.component_id<=255:parser.error('IDs must be 1–255')
    try:
        from pymavlink import mavutil
    except ImportError:
        parser.exit(1,'Install: python -m pip install -r tools/requirements-mavlink.txt\n')
    # Explicit MAVLink 2 dialect; not dependent on caller environment variables.
    from pymavlink.dialects.v20 import common
    connection=mavutil.mavlink_connection(args.connect,baud=args.baud,
        source_system=args.system_id,source_component=args.component_id)
    connection.mav=common.MAVLink(connection,srcSystem=args.system_id,srcComponent=args.component_id)
    connection.mav.robust_parsing=True
    next_heartbeat=0
    try:
        while True:
            now=time.monotonic()
            if not args.passive and now>=next_heartbeat:
                connection.mav.heartbeat_send(common.MAV_TYPE_GCS,common.MAV_AUTOPILOT_INVALID,0,0,common.MAV_STATE_ACTIVE)
                next_heartbeat=now+1
            message=connection.recv_match(blocking=True,timeout=.2)
            if not message or message.get_type()!='TUNNEL' or message.payload_type!=0:continue
            if message.target_system not in (0,args.system_id) or message.target_component not in (0,args.component_id):continue
            try:record=decode_payload(message.payload,message.payload_length)
            except ValueError as exc:
                print(json.dumps({'kind':'decode_error','error':str(exc)}),flush=True);continue
            if record:
                record.update(mavlink_system_id=message.get_srcSystem(),mavlink_component_id=message.get_srcComponent(),
                    host_received_utc=datetime.now(timezone.utc).isoformat())
                print(json.dumps(record,allow_nan=False),flush=True)
    except KeyboardInterrupt:pass
    finally:connection.close()


if __name__=='__main__':main()
