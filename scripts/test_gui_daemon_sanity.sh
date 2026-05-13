#!/bin/bash

# One-command sanity script (mirrors GUI launch)
# Tests the app-bundled daemon with unified ports

echo "🔧 **GUI DAEMON SANITY CHECK**"
echo "=============================="
echo ""

BASE="$HOME/.dinero/regtest"
DAEMON_PATH="${DAEMON_PATH:-$(pwd)/build/dinerod}"

echo "**Step 1: Kill any existing dinerod processes**"
pkill -f dinerod || true
sleep 1

echo "**Step 2: Ensure directories exist**"
mkdir -p "$BASE"

echo "**Step 3: Launch daemon with exact GUI parameters**"
echo "Command: $DAEMON_PATH --regtest --datadir=\"$BASE\" --rpcport=20996 --wsport=18881 --printtoconsole"
echo ""

"$DAEMON_PATH" \
  --regtest --datadir="$BASE" --rpcport=20996 --wsport=18881 --printtoconsole \
  > "$BASE/gui_launch.log" 2>&1 &

DAEMON_PID=$!
echo "Daemon started with PID: $DAEMON_PID"

echo "**Step 4: Wait 3 seconds for daemon startup**"
sleep 3

echo "**Step 5: Check port listener**"
if lsof -nP -iTCP:20996 | grep LISTEN; then
    echo "✅ Port 20996 is listening"
else
    echo "❌ No listener on port 20996 (bad)"
fi

echo "**Step 6: Check cookie file**"
if test -f "$BASE/.cookie"; then
    echo "✅ Cookie file exists: $BASE/.cookie"
else
    echo "❌ No cookie file (bad)"
fi

echo "**Step 7: Test health endpoint**"
if curl -s http://127.0.0.1:20996/healthz | grep -q "ok"; then
    echo "✅ Health endpoint responds OK"
else
    echo "❌ Health endpoint failed"
fi

echo "**Step 8: Check daemon log (first 20 lines)**"
echo "Log file: $BASE/gui_launch.log"
echo "---"
head -20 "$BASE/gui_launch.log"
echo "---"

echo ""
echo "**Sanity check complete!**"
echo "Daemon PID: $DAEMON_PID (kill with: kill $DAEMON_PID)"
echo "Log file: $BASE/gui_launch.log"
echo "Cookie: $BASE/.cookie"
echo ""

# Keep daemon running for GUI testing
echo "Daemon is still running for GUI testing..."
echo "Kill it manually when done: kill $DAEMON_PID"
