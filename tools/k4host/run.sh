#!/usr/bin/env bash
# Every scenario of k4host (see main.c), each against a mock K4 of its own
# (tools/mock_k4.py) -- the radio's faults are the scenario's -- and a client
# of its own: radio_start() runs once in a firmware's life.
set -u
cd "$(dirname "$0")"
LOGS=${K4HOST_LOGS:-logs}
mkdir -p "$LOGS"
port=${K4HOST_PORT:-29200}
fails=0

run() {                                  # run <scenario> "<the mock's options>"
    local name=$1 opts=$2
    port=$((port + 1))
    # shellcheck disable=SC2086
    python3 ../mock_k4.py --port $port $opts > "$LOGS/$name.mock.log" 2>&1 &
    local mock=$!
    for _ in $(seq 50); do
        (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break
        sleep 0.1
    done
    if ASAN_OPTIONS=detect_leaks=0 timeout 60 ./k4host "$name" $port "$LOGS/$name.mock.log" \
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

run basic    ""
run refuse   "--refuse-tx"
run ignored  "--ignore-tx"
run late     "--late-tx 2.5"
run change   "--change-after 4"
run password ""
run silent   "--mute-pong"
echo "$fails scenario(s) failed"
exit $fails
