#!/usr/bin/env python3
"""Offline evidence probes, including known gaps; never open a real USB port."""

import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import bench
import dashboard


def report(check, **values):
    print(json.dumps({'check': check, 'result': 'observed', **values}, allow_nan=False))


def contact_eviction():
    contacts = dashboard.Contacts()

    def record(address):
        return {'kind': 'observation', 'source_address': address,
                'transport': 'wifi_beacon', 'rssi_dbm': -50,
                'decode_status': 'ok', 'message_type': 'basic_id',
                'data': {'uas_id_text': 'CLAIMED-ID'}}

    original = '02:00:00:00:00:00'
    contacts.ingest(record(original), 1000)
    for i in range(1, dashboard.MAX_CONTACTS + 1):
        contacts.ingest(record(f'02:00:00:00:{i >> 8:02x}:{i & 255:02x}'), 1000 + i / 1000)
    assert len(contacts.items) == dashboard.MAX_CONTACTS
    evicted = all(item['source_address'] != original for item in contacts.items.values())
    assert evicted
    report('rotating_sources_evict_contact', bounded_contacts=len(contacts.items),
           original_source_evicted=evicted)


def malformed_usb_status():
    raw = (b'{"schema":1,"kind":"status","receiver_id":"102030405060",'
           b'"session":"test","free_heap_bytes":1e309}\n')
    decoder = bench.LineDecoder()
    assert decoder.feed(raw) == [] and decoder.invalid_lines == 1
    good = (json.dumps(dict(schema=1,kind='status',receiver_id='102030405060',session='recovered',request_id='',
        role='rx',firmware='0.5.2',configuration_version=1,broadcast_id='SCOUT-01',hello_hex='',
        tx_transport='wifi_beacon',radio_ready=True,uptime_us=100))+'\n').encode()

    class FakeStream:
        reads = 0
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def write(self, data): return len(data)
        def read(self, size):
            self.reads += 1
            if self.reads == 1: return raw
            if self.reads == 2: return good
            device.stop.set()
            return b''

    info = {'id': '102030405060', 'serial': '10:20:30:40:50:60',
            'port': '/offline/fake-usb', 'tty': '/offline/fake-usb'}
    with tempfile.TemporaryDirectory(prefix='droneid-threatmodel-') as directory:
        device = dashboard.Device(info, locator=lambda: {info['id']: info},
                                  log_dir=directory, start=False)
        with patch.object(bench, 'serial_modules', return_value=(SimpleNamespace(SerialException=OSError), None)), \
                patch.object(bench, 'connect', return_value=FakeStream()):
            device.reader()
        assert device.status['session'] == 'recovered'
        json.dumps(device.snapshot(), allow_nan=False)
        report('nonfinite_usb_status', decoder_accepted=False,
               reader_recovered=True, snapshot_not_json_serializable=False)


if __name__ == '__main__':
    contact_eviction()
    malformed_usb_status()
