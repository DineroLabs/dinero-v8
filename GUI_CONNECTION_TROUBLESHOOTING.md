# 🔧 GUI Connection Issue - Troubleshooting

## Problem: GUI Not Showing Green Connection

### What the GUI Expects:

1. **Local daemon running** on `http://127.0.0.1:20998` (testnet)
2. **Cookie file** at one of these locations:
   - `./data/.cookie`
   - `./data-main/.cookie`
   - `~/Library/Application Support/Dinero/.cookie` (macOS)
3. **3-second delay** before first connection attempt

## Quick Diagnostic Checklist:

### Step 1: Is the daemon running locally?
```bash
# Check if daemon is running on your Mac
ps aux | grep dinerod | grep -v grep

# Check if port 20998 is listening
lsof -i :20998

# Try manual RPC call
curl http://127.0.0.1:20998 -d '{"method":"getblockchaininfo"}' -H "Content-Type: application/json"
```

### Step 2: Does the cookie file exist?
```bash
# Check for cookie file
ls -la ./data/.cookie 2>/dev/null
ls -la ./data-main/.cookie 2>/dev/null
ls -la ~/Library/Application\ Support/Dinero/.cookie 2>/dev/null

# If found, check contents (should be username:password format)
cat ./data/.cookie 2>/dev/null
```

### Step 3: Check GUI logs
```bash
# Launch GUI from terminal to see connection logs
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt 2>&1 | grep -E "cookie|connection|RPC"
```

## Most Likely Issues:

### Issue #1: No Local Daemon Running ❌
**Symptom:** GUI shows "⚠️ Connection Failed"  
**Fix:**
```bash
# Start local daemon
./build/dinerod -datadir=./data -testnet

# Or use the servers (GUI will need remote cookies)
```

### Issue #2: Cookie File Missing ❌
**Symptom:** GUI shows "Unauthorized" or "Authentication failed"  
**Fix:**
```bash
# If daemon is running, cookie should be at:
./data/.cookie

# Or copy from server:
scp -i ~/.ssh/dinerola.key root@172.93.160.131:/root/DineroCoin/data/.cookie ./data/

# Make sure GUI datadir matches daemon datadir!
```

### Issue #3: GUI Connecting to Wrong Port ❌
**Symptom:** Connection timeout  
**Fix:** GUI defaults to port 20998 (testnet). If daemon is on different port, set env var:
```bash
export DINERO_RPC_URL="http://127.0.0.1:YOUR_PORT/"
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt
```

## Quick Fixes:

### Option A: Connect to Remote Server (DineroLA)
```bash
# 1. Copy cookie from server
scp -i ~/.ssh/dinerola.key root@172.93.160.131:/root/DineroCoin/data/.cookie ~/dinero-server.cookie

# 2. Set up SSH tunnel (daemon binds to 127.0.0.1 only)
ssh -i ~/.ssh/dinerola.key -L 20998:127.0.0.1:20998 root@172.93.160.131 -N &

# 3. Copy cookie to local expected location
mkdir -p ./data
cp ~/dinero-server.cookie ./data/.cookie

# 4. Launch GUI
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt
```

### Option B: Run Local Daemon
```bash
# 1. Start daemon locally
./build/dinerod -datadir=./data -testnet -rpcport=20998

# 2. Wait for cookie file to be created
sleep 2

# 3. Launch GUI
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt
```

### Option C: Point GUI to Server Directly (if RPC binds to 0.0.0.0)
```bash
# If daemon is configured with -rpcbind=0.0.0.0
export DINERO_RPC_URL="http://172.93.160.131:20998/"

# Copy cookie
scp -i ~/.ssh/dinerola.key root@172.93.160.131:/root/DineroCoin/data/.cookie ./data/

# Launch GUI
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt
```

## Expected Behavior:

✅ **Green Connection:**
- GUI shows "✅ Connected: http://127.0.0.1:20998/"
- Status bar is green
- Data refreshes every 5 seconds

⚠️ **Orange Connection:**
- GUI trying multiple servers
- Shows "🔄 Connected to: ..."

❌ **Red Connection:**
- GUI shows "⚠️ Connection failed"
- Cookie missing or daemon not running

## Debug Commands:

```bash
# Check what the GUI sees
export QT_LOGGING_RULES="*.debug=true"
./build/bin/dinero-qt.app/Contents/MacOS/dinero-qt 2>&1 | tee gui-debug.log

# Watch for:
# - "Loaded cookie from: ..."
# - "Using custom RPC server: ..."
# - "Configured X RPC servers"
```

## Current Server Status:

All 3 servers are running daemons on port 20998:
- **DineroLA:** 172.93.160.131:20998
- **DineroCA:** 96.9.226.98:20998
- **DineroVA:** 173.249.195.59:20998

But they bind to `127.0.0.1` (local only), so you need SSH tunnel or local daemon.

