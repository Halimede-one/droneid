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
DRONEID_TEST_FIXTURES="$test_build_dir/fixtures.jsonl" DRONEID_FC_FIXTURES="$test_build_dir/fc-fixtures.jsonl" \
    python3 -m unittest discover -s tests -v
