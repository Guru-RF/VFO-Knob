#!/usr/bin/env bash
# Every scenario of flexhost (see main.c), each against a mock FlexRadio of
# its own (tools/mock_flex.py) -- the radio's faults are the scenario's -- and
# a client of its own: radio_start() runs once in a firmware's life.
set -u
cd "$(dirname "$0")"
LOGS=${FLEXHOST_LOGS:-logs}
mkdir -p "$LOGS"
port=${FLEXHOST_PORT:-28990}
fails=0

run() {                                  # run <scenario> "<the mock's options>"
    local name=$1 opts=$2
    port=$((port + 1))
    # shellcheck disable=SC2086
    python3 ../mock_flex.py --port $port $opts > "$LOGS/$name.mock.log" 2>&1 &
    local mock=$!
    for _ in $(seq 50); do
        (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break
        sleep 0.1
    done
    if ASAN_OPTIONS=detect_leaks=0 timeout 60 ./flexhost "$name" $port "$LOGS/$name.mock.log" \
           > "$LOGS/$name.log" 2>&1; then
        echo "PASS  $name"
    else
        echo "FAIL  $name -- $LOGS/$name.log, $LOGS/$name.mock.log"
        grep '^FAIL' "$LOGS/$name.log" | sed 's/^/      /'
        fails=$((fails + 1))
    fi
    kill $mock 2>/dev/null
    wait $mock 2>/dev/null
}

run basic     ""
run refuse    "--refuse-apply"
run airrefuse "--refuse-apply --air-on-apply"
run change    "--change-after 6"
run other     ""
echo "$fails scenario(s) failed"
exit $fails
