#!/usr/bin/env bash
# End-to-end test of the OSCQuery HTTP transport (HttpServer), no Rack needed.
# Compiles the server with an echo handler, curls a few requests, and checks the
# responses and a clean shutdown.
#
# Requires: g++, curl.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${1:-7799}
BIN=$(mktemp -u /tmp/http_test.XXXX)
LOG=$(mktemp -u /tmp/http_test.XXXX.log)

echo "== compiling harness =="
g++ -std=c++11 -I src -pthread \
    test/http_test.cpp src/osc/HttpServer.cpp \
    -o "$BIN"

echo "== running server on :$PORT =="
"$BIN" "$PORT" > "$LOG" 2>&1 &
SRV=$!
sleep 0.5

echo "== curling requests =="
ROOT=$(curl -s "http://127.0.0.1:$PORT/")
HOST=$(curl -s "http://127.0.0.1:$PORT/?HOST_INFO")
LEAF=$(curl -s "http://127.0.0.1:$PORT/param/12/0")
# Confirm CORS + JSON content-type headers are present.
HEADERS=$(curl -s -D - -o /dev/null "http://127.0.0.1:$PORT/")
echo "  root: $ROOT"
echo "  host: $HOST"
echo "  leaf: $LEAF"

wait "$SRV"
echo "== server output =="
cat "$LOG"

pass=1
echo "$ROOT"    | grep -q '"path":"/"'            || { echo "FAIL: root path"; pass=0; }
echo "$HOST"    | grep -q '"query":"HOST_INFO"'   || { echo "FAIL: HOST_INFO query"; pass=0; }
echo "$LEAF"    | grep -q '"path":"/param/12/0"'  || { echo "FAIL: leaf path"; pass=0; }
echo "$HEADERS" | grep -qi 'Content-Type: application/json'    || { echo "FAIL: content-type"; pass=0; }
echo "$HEADERS" | grep -qi 'Access-Control-Allow-Origin: \*'   || { echo "FAIL: CORS"; pass=0; }
grep -q "stopped cleanly" "$LOG"                  || { echo "FAIL: clean shutdown"; pass=0; }

rm -f "$BIN" "$LOG"
if [ "$pass" = 1 ]; then echo "RESULT: PASS"; exit 0; else echo "RESULT: FAIL"; exit 1; fi
