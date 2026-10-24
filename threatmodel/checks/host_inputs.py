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
    records = bench.LineDecoder().feed(raw)
    assert len(records) == 1

    class FakeStream:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def write(self, data):
            return len(data)

        def read(self, size):
            if self.sent:
                # Bound the probe if the original defect has been fixed.
                raise AssertionError('Reader survived input; update the baseline probe.')
            self.sent = True
            return raw

        sent = False

    info = {'id': '102030405060', 'serial': '10:20:30:40:50:60',
            'port': '/offline/fake-usb', 'tty': '/offline/fake-usb'}
    with tempfile.TemporaryDirectory(prefix='droneid-threatmodel-') as directory:
        device = dashboard.Device(info, locator=lambda: {info['id']: info},
                                  log_dir=directory, start=False)
        try:
            # Call the actual reader synchronously, replacing its serial backend.
            # No thread, hardware, HTTP request, or persistent log is created.
            with patch.object(bench, 'serial_modules', return_value=(SimpleNamespace(SerialException=OSError), None)), \
                    patch.object(bench, 'connect', return_value=FakeStream()):
                try:
                    device.reader()
                except ValueError as exc:
                    reader_error = type(exc).__name__
                else:
                    raise AssertionError('Reader no longer raises; update the baseline probe.')
            try:
                json.dumps(device.snapshot(), allow_nan=False)
            except ValueError:
                snapshot_invalid = True
            else:
                snapshot_invalid = False
            assert snapshot_invalid
            report('nonfinite_usb_status', decoder_accepted=True,
                   exception_escapes_reader=reader_error, snapshot_not_json_serializable=snapshot_invalid)
        finally:
            device.log_handler.close()


if __name__ == '__main__':
    contact_eviction()
    malformed_usb_status()
