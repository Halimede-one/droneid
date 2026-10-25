#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_build_dir=$(mktemp -d /tmp/droneid-tests.XXXXXX)
trap 'rm -rf "$test_build_dir"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Icomponents/rid -Icomponents/opendroneid \
    tests/test_rid.c components/rid/rid.c components/rid/rid_wifi.c components/opendroneid/opendroneid.c \
    -lm -o "$test_build_dir/test_rid"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_rid" > "$test_build_dir/fixtures.jsonl"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Icomponents/fc_link -Icomponents/mavlink/include -Icomponents/rid -Icomponents/opendroneid \
    tests/test_fc_link.c components/fc_link/fc_link.c components/rid/rid.c components/opendroneid/opendroneid.c \
    -lm -o "$test_build_dir/test_fc_link"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_fc_link" > "$test_build_dir/fc-fixtures.jsonl"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Icomponents/uart_guard -Icomponents/mavlink/include -Icomponents/fc_link -Icomponents/rid -Icomponents/opendroneid \
    tests/test_uart_guard.c components/uart_guard/uart_guard.c components/uart_guard/guard_selftest.c components/fc_link/fc_link.c \
    -o "$test_build_dir/test_uart_guard"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_uart_guard" > "$test_build_dir/guard-fixtures.jsonl"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Ifc_filter -Icomponents/uart_guard -Icomponents/mavlink/include \
    tests/test_fc_filter.c components/uart_guard/uart_guard.c -o "$test_build_dir/test_fc_filter"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_fc_filter"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Ifc_filter -Icomponents/fc_link -Icomponents/mavlink/include -Icomponents/rid -Icomponents/opendroneid \
    tests/test_fc_filter_direct.c components/fc_link/fc_link.c -o "$test_build_dir/test_fc_filter_direct"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_fc_filter_direct" > "$test_build_dir/fc-direct-fixtures.jsonl"
cc -std=c11 -O1 -g -Wno-address-of-packed-member -fsanitize=address,undefined \
    -Icomponents/fc_link -Icomponents/mavlink/include -Icomponents/rid -Icomponents/opendroneid \
    -c components/fc_link/fc_link.c -o "$test_build_dir/fc_link.o"
cc -std=c11 -O1 -g -Wno-address-of-packed-member -fsanitize=address,undefined \
    -Icomponents/uart_guard -Icomponents/mavlink/include \
    -c components/uart_guard/uart_guard.c -o "$test_build_dir/uart_guard.o"
c++ -std=c++11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-address-of-packed-member \
    -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
    -Ifc_filter -Icomponents/uart_guard -Icomponents/mavlink/include -Icomponents/fc_link -Icomponents/rid -Icomponents/opendroneid \
    tests/test_fc_filter_adapters.cpp "$test_build_dir/uart_guard.o" "$test_build_dir/fc_link.o" -o "$test_build_dir/test_fc_filter_adapters"
ASAN_OPTIONS=detect_leaks=0 "$test_build_dir/test_fc_filter_adapters"
DRONEID_TEST_FIXTURES="$test_build_dir/fixtures.jsonl" DRONEID_FC_FIXTURES="$test_build_dir/fc-fixtures.jsonl" \
DRONEID_GUARD_FIXTURES="$test_build_dir/guard-fixtures.jsonl" \
DRONEID_FC_DIRECT_FIXTURES="$test_build_dir/fc-direct-fixtures.jsonl" \
    python3 -m unittest discover -s tests -v
