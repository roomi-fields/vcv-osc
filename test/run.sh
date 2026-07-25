#!/usr/bin/env bash
# End-to-end test of the OSC layer, independent of Rack.
#
# 1. Compiles the OSC networking layer into a standalone server harness.
# 2. Runs it, self-testing serialize->parse.
# 3. Sends the real command surface from python-osc (proving wire-compatibility
#    with osc-bridge, which uses the same OSC 1.0 encoding) and checks it is
#    received and parsed with correct argument types.
#
# Requires: g++, python3 with python-osc (pip install python-osc).
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${1:-7770}
BIN=$(mktemp -u /tmp/osc_roundtrip.XXXX)
LOG=$(mktemp -u /tmp/osc_rx.XXXX.log)

echo "== compiling harness =="
g++ -std=c++11 -I. -pthread \
    test/osc_roundtrip_test.cpp src/osc/OscServer.cpp src/osc/UdpSocket.cpp \
    -o "$BIN"

echo "== running server on :$PORT =="
"$BIN" "$PORT" > "$LOG" 2>&1 &
SRV=$!
sleep 0.6

echo "== sending command surface via python-osc =="
python3 - "$PORT" <<'PY'
import sys, time
from pythonosc.udp_client import SimpleUDPClient
from pythonosc.osc_bundle_builder import OscBundleBuilder, IMMEDIATELY
from pythonosc.osc_message_builder import OscMessageBuilder
c = SimpleUDPClient("127.0.0.1", int(sys.argv[1]))
for addr, args in [("/ping", []), ("/param/set", [12345, 3, 0.75]),
                   ("/param/get", [12345, 3]), ("/cable/add", [1, 0, 2, 1]),
                   ("/cable/remove", [2, 1]), ("/state/dump", [1]),
                   ("/param/watch", [12345, 3, 1]),
                   # Phase 1 symbolic addressing: module & param by name (strings),
                   # plus the structural-watch subscription.
                   ("/param/set", ["Fundamental/VCO", "Frequency", 0.5]),
                   ("/cable/add", ["VCO", "Sine", "VCF", "Audio"]),
                   ("/state/watch", [1]),
                   # Phase 2 RESTful per-address form (OSCQuery leaf address).
                   ("/param/12/0", [0.8]),
                   # Phase 3 module lifecycle + presets.
                   ("/module/add", ["Fundamental", "VCO", 100.0, 50.0]),
                   ("/module/remove", [42]),
                   ("/module/preset_save", [42, "/tmp/p.vcvm"]),
                   ("/module/preset_load", ["VCF", "/tmp/p.vcvm"])]:
    c.send_message(addr, args)
bb = OscBundleBuilder(IMMEDIATELY)
for addr, args in [("/param/set", [9, 9, 0.1]), ("/param/set", [9, 10, 0.2])]:
    mb = OscMessageBuilder(address=addr)
    for a in args:
        mb.add_arg(a)
    bb.add_content(mb.build())
c.send(bb.build())
time.sleep(0.3)
print("sent 15 messages + 1 bundle(2)")
PY

wait "$SRV"
echo "== server output =="
cat "$LOG"

if grep -q "17 messages received" "$LOG" \
   && grep -q 'RX /param/set i:12345 i:3 f:0.75' "$LOG" \
   && grep -q 'RX /param/set s:"Fundamental/VCO" s:"Frequency" f:0.5' "$LOG" \
   && grep -q 'RX /cable/add s:"VCO" s:"Sine" s:"VCF" s:"Audio"' "$LOG" \
   && grep -q 'RX /param/12/0 f:0.8' "$LOG" \
   && grep -q 'RX /module/add s:"Fundamental" s:"VCO" f:100 f:50' "$LOG"; then
    echo "RESULT: PASS"
    rc=0
else
    echo "RESULT: FAIL"
    rc=1
fi
rm -f "$BIN" "$LOG"
exit $rc
