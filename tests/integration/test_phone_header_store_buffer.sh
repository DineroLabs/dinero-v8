#!/usr/bin/env bash
# RocksDB preallocates the header store's write-ahead log at 1.1 x
# write_buffer_size. With the 64 MB default a phone reserved ~70 MB of disk
# for a log holding a few KB. The ios_utreexo profile must open the header
# store with a 4 MB buffer; mac_fullblock (desktop dinerod, macOS DineroDPI)
# keeps the default. RocksDB records the options it opened with in OPTIONS-*.
# Usage: DINEROD=/path/to/dinerod test_phone_header_store_buffer.sh
set -euo pipefail
DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
[[ -x "$DINEROD" ]] || { echo "dinerod not executable at $DINEROD"; exit 1; }
BASE_PORT="${BASE_PORT:-37600}"
WORK="$(mktemp -d -t dinero_header_buffer_XXXXXX)"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
cleanup() {
    pkill -f "datadir=$WORK" 2>/dev/null || true; sleep 1; pkill -9 -f "datadir=$WORK" 2>/dev/null || true
    if [[ "$KEEP_ON_FAIL" != "1" ]]; then rm -rf "$WORK"; else echo "[INFO] kept $WORK" >&2; fi
}
trap cleanup EXIT
fail() { printf '[FAIL] %s\n' "$*" >&2; KEEP_ON_FAIL=1; exit 1; }

header_buffer() {  # <profile> <port offset> -> write_buffer_size the header store opened with
    local dir="$WORK/$1" rpc=$((BASE_PORT + $2)) i
    mkdir -p "$dir"
    "$DINEROD" --regtest --datadir="$dir" --rpcport="$rpc" --port=$((rpc + 100)) \
        --wallet-socket-port=$((rpc + 200)) --listen=0 --dnsseed=0 --fixedseeds=0 \
        --sync-profile="$1" > "$dir/daemon.log" 2>&1 &
    for i in $(seq 1 90); do ls "$dir"/headers/OPTIONS-* >/dev/null 2>&1 && break; sleep 1; done
    pkill -f "datadir=$dir" 2>/dev/null || true
    for i in $(seq 1 60); do pgrep -f "datadir=$dir" >/dev/null 2>&1 || break; sleep 0.5; done
    local options; options="$(ls -t "$dir"/headers/OPTIONS-* 2>/dev/null | head -1)"
    [[ -n "$options" ]] || fail "$1: header store wrote no OPTIONS file"
    awk -F= '/^\[CFOptions "default"\]/{cf=1} cf && $1~/^ *write_buffer_size$/{print $2; exit}' "$options" | tr -d ' '
}

phone="$(header_buffer ios_utreexo 0)"
[[ "$phone" == "4194304" ]] || fail "ios_utreexo header store write_buffer_size=$phone, want 4194304"
echo "[PASS] ios_utreexo header store opens with a 4 MB write buffer"
desktop="$(header_buffer mac_fullblock 1)"
[[ "$desktop" == "67108864" ]] || fail "mac_fullblock header store write_buffer_size=$desktop, want the 64 MB default"
echo "[PASS] mac_fullblock header store keeps the 64 MB default"
