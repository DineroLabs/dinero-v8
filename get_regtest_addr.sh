#!/bin/bash
ps aux | grep dinerod | grep -v grep | awk '{print $2}' | xargs kill -9 2>/dev/null
TEST_DIR="/tmp/addr-test-$$"
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"
./build/dinerod --regtest --datadir="$TEST_DIR" > "$TEST_DIR/daemon.log" 2>&1 &
DPID=$!
sleep 5
COOKIE=$(cat "$TEST_DIR/.cookie")
ADDR=$(curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":[]}' | grep -o 'rdin1[^"]*')
echo "Valid regtest address: $ADDR"
kill $DPID 2>/dev/null
