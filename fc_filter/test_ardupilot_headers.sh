#!/usr/bin/env bash
set -euo pipefail
fc_checkout=$(realpath "${1:?Usage: bash fc_filter/test_ardupilot_headers.sh PATH_TO_PATCHED_ARDUPILOT}")
cd "$(dirname "$0")/.."
python3 fc_filter/install.py ardupilot "$fc_checkout" --verify
fc_headers="$fc_checkout/build/MicoAir743v2/libraries/GCS_MAVLink/include/mavlink/v2.0"
test -f "$fc_headers/all/mavlink.h"
fc_test_dir=$(mktemp -d /tmp/droneid-fc-vendor.XXXXXX)
trap 'rm -rf "$fc_test_dir"' EXIT
fc_flags=(-O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie
    -Ifc_filter -Icomponents/uart_guard -Icomponents/fc_link -Icomponents/rid -Icomponents/opendroneid \
    -I"$fc_headers" -include all/mavlink.h)
cc -std=c11 "${fc_flags[@]}" -c components/uart_guard/uart_guard.c -o "$fc_test_dir/guard.o"
cc -std=c11 "${fc_flags[@]}" tests/test_fc_filter.c "$fc_test_dir/guard.o" -o "$fc_test_dir/policy"
ASAN_OPTIONS=detect_leaks=0 "$fc_test_dir/policy"
cc -std=c11 "${fc_flags[@]}" -c components/fc_link/fc_link.c -o "$fc_test_dir/radio.o"
cc -std=c11 "${fc_flags[@]}" tests/test_fc_filter_direct.c "$fc_test_dir/radio.o" -o "$fc_test_dir/direct"
ASAN_OPTIONS=detect_leaks=0 "$fc_test_dir/direct" > "$fc_test_dir/fixtures.jsonl"
c++ -std=c++11 "${fc_flags[@]}" tests/test_fc_filter_adapters.cpp "$fc_test_dir/guard.o" "$fc_test_dir/radio.o" -o "$fc_test_dir/adapters"
ASAN_OPTIONS=detect_leaks=0 "$fc_test_dir/adapters"
