# Dell Tower Diagnosis Guide

**Purpose:** Systematically diagnose and fix issues with DineroCoin node on Dell tower Linux server.

**Date:** 2026-01-07
**Status:** Initial diagnostic phase

---

## Quick Start

### Run Automated Diagnostic

```bash
cd ~/DineroCoin  # or wherever your repo is
./tools/diagnose_node.sh
```

Save output for analysis:
```bash
./tools/diagnose_node.sh > diagnostic_report.txt 2>&1
```

---

## Manual Diagnostic Steps

### Step 1: Basic System Check

```bash
# Check OS and kernel
uname -a
cat /etc/os-release

# Check available resources
free -h
df -h
nproc  # CPU cores

# Check system load
uptime
top -n 1 -b | head -20
```

**What to look for:**
- [ ] Sufficient RAM (minimum 2GB, recommended 4GB+)
- [ ] Sufficient disk space (minimum 20GB free)
- [ ] System not overloaded (load average < number of CPU cores)

---

### Step 2: Build Verification

```bash
cd ~/DineroCoin  # adjust path as needed

# Check if code is latest
git status
git log --oneline -5

# Check if binaries exist
ls -lh build/dinerod build/dinero-cli build/dinero-miner

# Check binary dependencies
ldd build/dinerod | grep "not found"
```

**What to look for:**
- [ ] Binaries exist and are recent (not months old)
- [ ] No missing shared libraries
- [ ] Git status is clean (or has expected changes)

**If binaries are missing or old:**
```bash
# Clean rebuild
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

### Step 3: Configuration Check

```bash
# Check if config exists
ls -la ~/.dinero/

# View config (if exists)
cat ~/.dinero/dinero.conf

# Check if cookie exists (means daemon ran at least once)
ls -la ~/.dinero/.cookie
```

**Expected files in ~/.dinero/:**
- `.cookie` - RPC authentication (created when daemon starts)
- `dinero.conf` - Configuration (optional, uses defaults if missing)
- `debug.log` - Daemon log file
- `blocks/` - Blockchain data
- `chainstate/` - UTXO database

**If dinero.conf doesn't exist, create one:**
```bash
cat > ~/.dinero/dinero.conf <<EOF
# DineroCoin Configuration
server=1
daemon=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1

# Logging
debug=net
debug=rpc
printtoconsole=0

# Network
listen=1
maxconnections=125

# Performance
dbcache=4096
maxmempool=300
EOF
```

---

### Step 4: Daemon Status Check

```bash
# Check if dinerod is running
ps aux | grep dinerod
pgrep -a dinerod

# If running, check how long
ps -p $(pgrep dinerod) -o etime=

# Check listening ports
netstat -lntp 2>/dev/null | grep dinerod
# OR
ss -lntp | grep dinerod
```

**Expected ports:**
- `20997` - RPC port (should listen on 127.0.0.1 only)
- `20999` - P2P port (should listen on 0.0.0.0 or specific IP)

**If daemon is NOT running:**
```bash
# Try to start manually (foreground for debugging)
cd ~/DineroCoin
./build/dinerod -printtoconsole -debug=net -debug=rpc

# Watch for errors in output
# Press Ctrl+C to stop

# If it starts successfully, run as daemon:
./build/dinerod -daemon
```

**If daemon keeps crashing:**
```bash
# Check debug log for errors
tail -100 ~/.dinero/debug.log

# Look for specific errors:
grep -i "error" ~/.dinero/debug.log | tail -20
grep -i "exception" ~/.dinero/debug.log | tail -20
grep -i "assertion" ~/.dinero/debug.log | tail -20
```

---

### Step 5: RPC Connectivity Test

```bash
# Test basic RPC call
./build/dinero-cli getblockchaininfo

# If that fails, try with explicit cookie auth
COOKIE=$(cat ~/.dinero/.cookie)
curl -s --user "$COOKIE" \
  --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20997/

# Check RPC port is listening
curl -v http://127.0.0.1:20997/ 2>&1 | grep Connected
```

**Common RPC errors:**

**Error: "Could not connect to the server"**
→ Daemon not running or wrong port
```bash
# Check if daemon is actually running
pgrep dinerod
# Check what port it's using
netstat -lntp 2>/dev/null | grep dinerod
```

**Error: "Unauthorized"**
→ Cookie or credentials wrong
```bash
# Verify cookie exists and is readable
cat ~/.dinero/.cookie
# Cookie format: __cookie__:<random_string>
```

**Error: "Connection refused"**
→ RPC not enabled or firewall blocking
```bash
# Check firewall
sudo iptables -L -n | grep 20997
sudo ufw status
```

---

### Step 6: Peer Connection Test

```bash
# Check peer count
./build/dinero-cli getconnectioncount

# Get detailed peer info
./build/dinero-cli getpeerinfo

# Check for banned peers
./build/dinero-cli listbanned
```

**If peer count is 0:**

```bash
# Check if P2P port is open
netstat -ln | grep 20999
ss -ln | grep 20999

# Check firewall
sudo ufw status
sudo iptables -L -n | grep 20999

# Try manual peer connection (use another node's IP if available)
./build/dinero-cli addnode "IP_ADDRESS:20999" add

# Check if it connected
./build/dinero-cli getpeerinfo
```

**To allow P2P through firewall:**
```bash
# UFW (Ubuntu/Debian)
sudo ufw allow 20999/tcp

# firewalld (RHEL/CentOS)
sudo firewall-cmd --permanent --add-port=20999/tcp
sudo firewall-cmd --reload

# iptables (manual)
sudo iptables -A INPUT -p tcp --dport 20999 -j ACCEPT
sudo iptables-save > /etc/iptables/rules.v4
```

---

### Step 7: Network Connectivity Test

```bash
# Test internet connectivity
ping -c 3 8.8.8.8

# Test DNS
ping -c 3 google.com

# Test if we can reach common Bitcoin/crypto nodes (proxy test)
nc -zv 1.1.1.1 443

# Check routing
ip route
netstat -rn
```

**If no internet:**
```bash
# Check network interface
ip addr show
ifconfig

# Check if gateway is reachable
ping -c 3 $(ip route | grep default | awk '{print $3}')

# Check DNS
cat /etc/resolv.conf
```

---

### Step 8: Blockchain Sync Status

```bash
# Check blockchain info
./build/dinero-cli getblockchaininfo

# Expected output (mainnet, height 0):
# {
#   "chain": "main",
#   "blocks": 0,
#   "headers": 0,
#   "bestblockhash": "00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74",
#   ...
# }

# Check if syncing
./build/dinero-cli getblockchaininfo | grep -E "blocks|headers|verificationprogress"

# If headers > blocks, it's syncing
# If verificationprogress < 1.0, it's syncing
```

**Sync troubleshooting:**

**Stuck at block 0:**
```bash
# Check if isolated (no peers)
./build/dinero-cli getconnectioncount
# If 0, you need peer connections first

# Check debug log
tail -50 ~/.dinero/debug.log | grep -i "block\|sync\|peer"
```

**Slow sync:**
```bash
# Increase database cache
echo "dbcache=4096" >> ~/.dinero/dinero.conf
./build/dinero-cli stop
sleep 5
./build/dinerod -daemon

# Check disk I/O
iostat -x 1 5

# Check if disk is slow
dd if=/dev/zero of=~/test.tmp bs=1M count=1024 oflag=direct
# Should be > 50 MB/s for SSD, > 10 MB/s for HDD
rm ~/test.tmp
```

---

### Step 9: Log Analysis

```bash
# View live log
tail -f ~/.dinero/debug.log

# Search for specific issues
grep -i "error" ~/.dinero/debug.log | tail -20
grep -i "warning" ~/.dinero/debug.log | tail -20
grep -i "assertion" ~/.dinero/debug.log | tail -20

# Check connection attempts
grep "connection" ~/.dinero/debug.log | tail -20

# Check sync progress
grep "UpdateTip" ~/.dinero/debug.log | tail -20
```

**Common log messages:**

**GOOD:**
```
UpdateTip: new best=000008e4d5db... height=0
connection from 192.168.1.x:12345 accepted
received: version
```

**BAD:**
```
ERROR: AcceptBlock: bad-blk-length
ERROR: CheckProofOfWork(): hash doesn't match nBits
socket send error: Connection reset by peer
```

---

### Step 10: Multi-Node Local Test

**Test if two local nodes can connect to each other:**

```bash
# Terminal 1: Start node 1
./build/dinerod -daemon -datadir=~/.dinero-node1 -port=20999 -rpcport=20997

# Terminal 2: Start node 2
./build/dinerod -daemon -datadir=~/.dinero-node2 -port=21000 -rpcport=21001

# Terminal 3: Connect them
sleep 5  # wait for startup
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21000" add

# Check if connected
./build/dinero-cli -datadir=~/.dinero-node1 getpeerinfo
./build/dinero-cli -datadir=~/.dinero-node2 getpeerinfo

# Should see 1 peer on each node
```

**If this works:** Network/P2P code is functional, problem is external connectivity

**If this fails:** Problem with P2P code or local network configuration

---

## Common Issues & Solutions

### Issue 1: "Cannot connect to server"

**Symptoms:**
- `dinero-cli` commands fail
- Error: "Could not connect to the server"

**Solutions:**
1. Check daemon is running: `pgrep dinerod`
2. Start daemon if not running: `./build/dinerod -daemon`
3. Wait 10 seconds for startup
4. Check debug log: `tail -20 ~/.dinero/debug.log`

---

### Issue 2: "No peer connections"

**Symptoms:**
- `getconnectioncount` returns 0
- Node is isolated

**Solutions:**
1. Check firewall: `sudo ufw status`
2. Allow P2P port: `sudo ufw allow 20999/tcp`
3. Check if port is open: `netstat -ln | grep 20999`
4. Manually add peer: `./build/dinero-cli addnode IP:20999 add`
5. For testing, try local multi-node setup (see Step 10)

---

### Issue 3: "Daemon keeps crashing"

**Symptoms:**
- `dinerod` exits immediately after start
- Process disappears from `ps aux`

**Solutions:**
1. Run in foreground to see error: `./build/dinerod -printtoconsole`
2. Check system resources: `free -h` and `df -h`
3. Check for corrupted database:
   ```bash
   ./build/dinerod -reindex
   ```
4. Last resort - fresh start:
   ```bash
   ./build/dinero-cli stop
   mv ~/.dinero ~/.dinero.backup
   ./build/dinerod -daemon
   ```

---

### Issue 4: "Sync is stuck"

**Symptoms:**
- `blocks` and `headers` don't increase
- `verificationprogress` stuck at 0.0

**Solutions:**
1. Check peer count: `./build/dinero-cli getconnectioncount`
   - If 0, you need peers first
2. Check if blocks are being received:
   ```bash
   tail -f ~/.dinero/debug.log | grep -i "block\|height"
   ```
3. Restart daemon:
   ```bash
   ./build/dinero-cli stop
   sleep 5
   ./build/dinerod -daemon
   ```

---

### Issue 5: "High CPU usage"

**Symptoms:**
- `dinerod` using 100% CPU constantly
- System is slow

**Solutions:**
1. Check what it's doing:
   ```bash
   strace -p $(pgrep dinerod) -c
   ```
2. Could be syncing (normal, temporary)
3. Could be verifying (normal, temporary)
4. Reduce work:
   ```bash
   echo "par=2" >> ~/.dinero/dinero.conf  # reduce verification threads
   ./build/dinero-cli stop && ./build/dinerod -daemon
   ```

---

### Issue 6: "Disk space full"

**Symptoms:**
- Daemon stops
- Error: "No space left on device"

**Solutions:**
1. Check disk usage: `df -h`
2. Clean up old logs:
   ```bash
   truncate -s 0 ~/.dinero/debug.log
   ```
3. Enable pruning (if blockchain is large):
   ```bash
   echo "prune=5000" >> ~/.dinero/dinero.conf
   ./build/dinero-cli stop && ./build/dinerod -daemon
   ```

---

## Diagnostic Checklist

Use this checklist when troubleshooting:

### Hardware/OS
- [ ] Sufficient RAM (4GB+ recommended)
- [ ] Sufficient disk space (20GB+ free)
- [ ] CPU not overloaded (load < cores)
- [ ] Disk not failing (check SMART status)

### Build
- [ ] Latest code from git
- [ ] Binaries compiled successfully
- [ ] No missing shared libraries
- [ ] Correct architecture (x86_64 or ARM)

### Configuration
- [ ] Data directory exists (~/.dinero)
- [ ] Config file correct (if exists)
- [ ] Permissions OK (user can write)

### Network
- [ ] Internet connectivity working
- [ ] DNS resolution working
- [ ] Firewall allows P2P port (20999)
- [ ] No port conflicts

### Daemon
- [ ] Process is running
- [ ] Not crashing repeatedly
- [ ] Listening on correct ports
- [ ] RPC responding

### Peers
- [ ] At least 1 peer connected
- [ ] No banned peers blocking connections
- [ ] P2P messages being exchanged

### Sync
- [ ] Genesis hash correct (000008e4d5db...)
- [ ] Headers downloading
- [ ] Blocks downloading
- [ ] No consensus errors

---

## Reporting Issues

When asking for help, include:

1. **System Info:**
   ```bash
   uname -a
   cat /etc/os-release
   free -h
   df -h
   ```

2. **Build Info:**
   ```bash
   git log --oneline -1
   ls -lh build/dinerod
   ldd build/dinerod | grep "not found"
   ```

3. **Daemon Status:**
   ```bash
   ps aux | grep dinerod
   ./build/dinero-cli getblockchaininfo
   ./build/dinero-cli getconnectioncount
   ```

4. **Recent Logs:**
   ```bash
   tail -100 ~/.dinero/debug.log
   ```

5. **Diagnostic Output:**
   ```bash
   ./tools/diagnose_node.sh > report.txt 2>&1
   ```

---

## Next Steps After Diagnosis

### If Everything Works Locally:
1. ✅ Document your working configuration
2. ✅ Run for 7+ days to ensure stability
3. ✅ Test multi-node setup locally
4. ✅ Prepare for external deployment

### If Issues Found:
1. 🔧 Fix issues one by one (use solutions above)
2. 🔧 Test after each fix
3. 🔧 Document what you changed
4. 🔧 Repeat diagnostic

### Once Stable:
1. 📝 Write deployment guide (based on what worked)
2. 📝 Create startup scripts
3. 📝 Set up monitoring
4. 🚀 Deploy to external server

---

**Good luck with the diagnosis! Report back with the output of `diagnose_node.sh` and we'll solve any issues found.**
