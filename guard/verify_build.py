#!/usr/bin/env python3
"""Verify a local guard build; this is not deployed-device attestation."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from guard_bench import GuardFirmware, PROFILES

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--board', choices=PROFILES, default='xiao_esp32c5')
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
try:
    result = GuardFirmware(args.board, args.build_dir).bundle()
except (OSError, ValueError, KeyError, TypeError) as exc:
    parser.error(str(exc))
print(json.dumps({k:v for k,v in result.items() if k != 'images'} | {
    'radio_network_components':[], 'compiler_stack_protection':'all',
    'scope':'Local build metadata/configuration only; not signature or device attestation.'}, indent=2))
