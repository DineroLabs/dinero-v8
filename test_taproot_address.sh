#!/bin/bash
# Simple test to check if Taproot address generation works

DATADIR="/tmp/taproot-address-test"
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

echo "Starting daemon..."
./bin/dinerod --regtest --datadir="$DATADIR" --rpcport=21999 --p2pport=21998 --no-stratum > "$DATADIR/daemon.log" 2>&1 &
DPID=$!

sleep 8

echo "Generating Taproot address..."
./bin/dinero-cli -datadir="$DATADIR" -rpcport=21999 getnewaddress 2>&1 | tee "$DATADIR/addr_output.txt"

sleep 2

echo ""
echo "Checking database..."
sqlite3 "$DATADIR/wallets/wallet_default.db" "SELECT address, type, idx FROM addresses;" 2>&1

echo ""
echo "Checking daemon log for INSERT messages..."
grep "💾" "$DATADIR/daemon.log" 2>&1 | tail -10

kill $DPID 2>/dev/null
rm -rf "$DATADIR"
