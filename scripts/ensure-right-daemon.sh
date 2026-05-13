#!/bin/bash
# ensure-right-daemon.sh - Verify daemon version and start it safely

set -euo pipefail

DAEMON_PATH="${1:-./bin/dinerod}"
EXPECTED_PREFIX="${2:-1.0.4}"
ADMIN_PORT="${3:-20999}"
RPC_PORT="${4:-20998}"

if [ ! -x "$DAEMON_PATH" ]; then
    echo "❌ Error: Daemon not found or not executable at $DAEMON_PATH"
    exit 1
fi

echo "🛡️  Version Guard: Checking daemon version..."

# Get version from daemon
DAEMON_VERSION=$("$DAEMON_PATH" --version | head -1 | cut -d' ' -f3)

echo "✅ Daemon path: $DAEMON_PATH"
echo "✅ Expected prefix: $EXPECTED_PREFIX"
echo "✅ Daemon version: $DAEMON_VERSION"

# Assert version prefix
if ! echo "$DAEMON_VERSION" | grep -q "^$EXPECTED_PREFIX"; then
    echo "❌ Error: Version mismatch. Expected '$EXPECTED_PREFIX', got '$DAEMON_VERSION'"
    exit 2
fi

echo "✅ Version assertion passed!"

# Kill any existing daemon
echo "🧹 Cleaning up existing daemon..."
pkill -f "$(basename "$DAEMON_PATH")" || true
sleep 1

# Start daemon with version assertion
echo "🚀 Starting daemon with version guard..."
exec "$DAEMON_PATH" \
    --assert-version-prefix="$EXPECTED_PREFIX" \
    -regtest \
    -dev-easypow \
    -gen \
    -genthreads=1 \
    -rpcport="$RPC_PORT" \
    -adminport="$ADMIN_PORT" \
    -wsport="$((ADMIN_PORT + 1))" \
    -printtoconsole=1 \
    -logtofile=0 \
    -datadir=./test-data/e2e-test
