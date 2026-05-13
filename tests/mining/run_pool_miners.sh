#!/bin/bash
# DineroCoin Multi-Miner Pool Test
# Spawns multiple Stratum miners in parallel

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MINER="$SCRIPT_DIR/stratum_miner.py"

# Defaults
POOL="${POOL:-127.0.0.1:3333}"
NUM_MINERS="${NUM_MINERS:-3}"
DURATION="${DURATION:-30}"
WALLET="${WALLET:-}"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --pool) POOL="$2"; shift 2 ;;
        --miners) NUM_MINERS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --wallet) WALLET="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --pool HOST:PORT   Pool address (default: 127.0.0.1:3333)"
            echo "  --miners N         Number of parallel miners (default: 3)"
            echo "  --duration SECS    Mining duration per miner (default: 30)"
            echo "  --wallet ADDR      Wallet address (required)"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$WALLET" ]]; then
    echo "Error: --wallet is required"
    exit 1
fi

echo "=========================================="
echo "DineroCoin Multi-Miner Pool Test"
echo "=========================================="
echo "Pool:     $POOL"
echo "Miners:   $NUM_MINERS"
echo "Duration: ${DURATION}s each"
echo "Wallet:   $WALLET"
echo "=========================================="
echo

# Store PIDs for cleanup
PIDS=()

cleanup() {
    echo "Stopping miners..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait
}
trap cleanup EXIT

# Start miners
for i in $(seq 1 $NUM_MINERS); do
    echo "Starting miner $i..."
    python3 "$MINER" \
        --pool "$POOL" \
        --wallet "$WALLET" \
        --worker "miner$i" \
        --duration "$DURATION" \
        > "/tmp/miner_${i}.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5  # Stagger connections
done

echo
echo "All $NUM_MINERS miners started. Waiting for completion..."
echo "Logs: /tmp/miner_*.log"
echo

# Wait for all miners
wait

echo
echo "=========================================="
echo "Test Complete - Miner Results"
echo "=========================================="

total_accepted=0
total_rejected=0
total_hashes=0

for i in $(seq 1 $NUM_MINERS); do
    log="/tmp/miner_${i}.log"
    if [[ -f "$log" ]]; then
        accepted=$(grep -o "Shares accepted:.*[0-9]*" "$log" | grep -o "[0-9]*$" || echo "0")
        rejected=$(grep -o "Shares rejected:.*[0-9]*" "$log" | grep -o "[0-9]*$" || echo "0")
        hashes=$(grep -o "Total hashes:.*[0-9,]*" "$log" | grep -o "[0-9,]*$" | tr -d ',' || echo "0")

        echo "Miner $i: accepted=$accepted, rejected=$rejected, hashes=$hashes"

        total_accepted=$((total_accepted + accepted))
        total_rejected=$((total_rejected + rejected))
        total_hashes=$((total_hashes + hashes))
    fi
done

echo "----------------------------------------"
echo "TOTAL: accepted=$total_accepted, rejected=$total_rejected, hashes=$total_hashes"
echo "=========================================="
