#!/usr/bin/env bash
# Snapshot probes, not release acceptance tests: some assert known weaknesses.
set -euo pipefail
cd "$(dirname "$0")/../.."
probe_build_dir=$(mktemp -d /tmp/droneid-threatmodel.XXXXXX)
trap 'rm -rf "$probe_build_dir"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Icomponents/fc_link -Icomponents/mavlink/include -Icomponents/rid -Icomponents/opendroneid \
    threatmodel/checks/boundaries.c components/fc_link/fc_link.c -o "$probe_build_dir/boundaries"
ASAN_OPTIONS=detect_leaks=0 "$probe_build_dir/boundaries"
python3 threatmodel/checks/host_inputs.py
