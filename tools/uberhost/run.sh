#!/usr/bin/env bash
# Every scenario of uberhost (see main.c), each against a mock UberSDR of its
# own (tools/mock_ubersdr.py) -- the receiver's options are the scenario's --
# and a client of its own: radio_start() runs once in a firmware's life.
set -u
cd "$(dirname "$0")"
LOGS=${UBERHOST_LOGS:-logs}
mkdir -p "$LOGS"
port=${UBERHOST_PORT:-28180}
fails=0

run() {                                  # run <scenario> "<the mock's options>"
    local name=$1 opts=$2
    port=$((port + 2))
    # shellcheck disable=SC2086
    python3 ../mock_ubersdr.py --port $port --kiwi-port $((port + 1)) $opts > "$LOGS/$name.mock.log" 2>&1 &
    local mock=$!
    for _ in $(seq 50); do
        (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null && break
        sleep 0.1
    done
    if ./uberhost "$name" $port $((port + 1)) > "$LOGS/$name.log" 2>&1; then
        echo "PASS  $name"
    else
        echo "FAIL  $name -- $LOGS/$name.log, $LOGS/$name.mock.log"
        grep '^FAIL' "$LOGS/$name.log" | sed 's/^/      /'
        fails=$((fails + 1))
    fi
    kill $mock 2>/dev/null
    wait $mock 2>/dev/null
}

run lan     ""
run timeup  "--guests-limited --time-limit 6"
run countdown "--guests-limited --time-limit 24"
run idle    "--guests-limited --time-limit 120 --idle-timeout 14"
run daylimit "--guests-limited --day-limit 16 --day-check 1"
run dayhold "--guests-limited --day-limit 8"
run restart "--guests-limited --time-limit 16"
run restartshut "--guests-limited --time-limit 16"
run wrongpw "--guests-limited --password secret"
run rightpw "--guests-limited --password secret"
run pwonly  "--guests-limited --password secret --password-only"
run kiwi    ""
run kiwisdr "--kiwi-flavour kiwisdr"
run kiwicw  "--kiwi-flavour kiwisdr --kiwi-cw 400,800"
echo "$fails scenario(s) failed"
exit $fails
