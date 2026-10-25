#!/usr/bin/env python3
"""Build an isolated guard profile from an activated ESP-IDF terminal."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent

def main():
    profiles = {p['id']: p for p in json.loads((ROOT/'boards.json').read_text()) if p['supported']}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True, choices=profiles)
    parser.add_argument('--menuconfig', action='store_true')
    args = parser.parse_args()
    if not os.environ.get('IDF_PATH'):
        parser.error('Run this in an ESP-IDF terminal (source export.sh first).')
    build = ROOT/'build'/args.board
    subprocess.run([sys.executable, str(Path(os.environ['IDF_PATH'])/'tools/idf.py'),
        '-C', str(ROOT), '-B', str(build), '-D', 'SDKCONFIG='+str(build/'sdkconfig'),
        '-D', 'IDF_TARGET='+profiles[args.board]['chip'],
        *(['menuconfig'] if args.menuconfig else []), 'build'], check=True)
    subprocess.run([sys.executable, str(ROOT/'verify_build.py'), '--board', args.board,
                    '--build-dir', str(build)], check=True)

if __name__ == '__main__':
    main()
