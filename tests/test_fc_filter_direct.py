import base64
import json
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from mavlink_receiver import decode_payload


class StandaloneFCFilterTests(unittest.TestCase):
    def test_real_radio_records_preserved_by_standalone_fc_filter(self):
        path = os.environ.get("DRONEID_FC_DIRECT_FIXTURES")
        if not path:
            self.skipTest("Run tests/run_host_tests.sh for actual radio/FC fixtures")
        try:
            from pymavlink.dialects.v20 import common
        except ImportError:
            self.skipTest("Install tools/requirements-mavlink.txt for reference decoder")
        fixtures = [json.loads(line) for line in Path(path).read_text().splitlines()]
        self.assertEqual(len(fixtures), 4)
        for fixture in fixtures:
            incoming = common.MAVLink(None).parse_char(bytes.fromhex(fixture["input_hex"]))
            frame = bytes.fromhex(fixture["frame_hex"])
            outgoing = common.MAVLink(None).parse_char(frame)
            self.assertFalse(any(byte in (0xfd, 0xfe) for byte in frame[1:]))
            self.assertEqual((outgoing.get_srcSystem(), outgoing.get_srcComponent()), (1, 242))
            self.assertEqual(outgoing.get_type(), incoming.get_type())
            if incoming.get_type() == "TUNNEL":
                raw = bytes(incoming.payload[:incoming.payload_length])
                encoded = bytes(outgoing.payload[:outgoing.payload_length])
                self.assertEqual(encoded[:4], b"DRG2")
                self.assertEqual(base64.b64decode(encoded[4:], validate=True), raw)
                original = decode_payload(raw, len(raw))
                normalized = decode_payload(encoded, len(encoded))
                self.assertEqual(normalized, original | {"wire_encoding": "drg2_base64"})
                if raw[5] == 1:  # Aircraft observations; health is receiver diagnostics.
                    self.assertEqual(normalized["authentication"], "unverified")


if __name__ == "__main__":
    unittest.main()
