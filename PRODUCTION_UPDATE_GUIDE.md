# 🚀 Dinero Production Server Update Guide
## Last 48 Hours: Critical Updates to Deploy

### 📋 What's New (Features to Deploy)

#### 1. **Signed ServerInfo with Node Identity** 🔐
- Persistent cryptographic node identity (secp256k1 keypair)
- Cryptographic signatures on serverinfo.json
- Privacy-preserving node IDs (HASH160)
- Files: `node_identity.h`, `node_identity.cpp`, `node_identity.dat`

#### 2. **HTTP /serverinfo Endpoint** 🌐
- Public endpoint: `GET /serverinfo` (no authentication)
- Enables external autodiscovery by registry
- Location: `http_rpc_server.cpp`

#### 3. **Auto-Refresh ServerInfo** ⏰
- Background thread updates serverinfo.json every 10 minutes
- Keeps metrics current without manual intervention
- Location: `main.cpp`

#### 4. **Multi-Asset System Enhancements** 💰
- Complete escrow system for cross-asset trades
- Asset metadata validation
- Persistent escrow storage across restarts
- RPC commands: `multiasset.createescrow`, `multiasset.release`, `multiasset.refund`

### 🏗️ Build Instructions

```bash
# 1. Navigate to project
cd ~/Documents/DineroCoin

# 2. Clean previous build (recommended)
rm -rf build
mkdir build

# 3. Configure with Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 4. Build daemon and CLI
cmake --build build --target dinerod dinero-cli

# 5. Verify binaries exist
ls -lh build/src/dinerod build/src/dinero-cli
```

**Expected output:**
```
-rwxr-xr-x  1 user  staff   25M Nov  4 04:00 build/src/dinerod
-rwxr-xr-x  1 user  staff   12M Nov  4 04:00 build/src/dinero-cli
```

### 🖥️ Server Configuration

Update this section with your actual server details:

```bash
# Virginia Node
VIRGINIA_HOST="173.249.195.59"
VIRGINIA_USER="root"
VIRGINIA_PATH="/opt/dinero"

# California Node
CALIFORNIA_HOST="172.93.160.131"
CALIFORNIA_USER="root"
CALIFORNIA_PATH="/opt/dinero"

# Mac Mini Node
MACMINI_HOST="192.168.1.100"
MACMINI_USER="admin"
MACMINI_PATH="/opt/dinero"
```

### 📦 Deployment Script

Save as `deploy_to_production.sh`:

```bash
#!/bin/bash
set -e  # Exit on any error

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
NODES=(
    "root@173.249.195.59:/opt/dinero"
    # "root@172.93.160.131:/opt/dinero"
    # "admin@192.168.1.100:/opt/dinero"
)

BUILD_DIR="$HOME/Documents/DineroCoin/build/src"
DAEMON_BIN="$BUILD_DIR/dinerod"
CLI_BIN="$BUILD_DIR/dinero-cli"

echo -e "${BLUE}╔══════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  Dinero Production Deployment Script        ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════╝${NC}"
echo ""

# Step 1: Verify local binaries exist
echo -e "${YELLOW}[1/6]${NC} Verifying local binaries..."
if [ ! -f "$DAEMON_BIN" ]; then
    echo -e "${RED}❌ ERROR: dinerod not found at $DAEMON_BIN${NC}"
    echo "Run: cmake --build build --target dinerod"
    exit 1
fi
if [ ! -f "$CLI_BIN" ]; then
    echo -e "${RED}❌ ERROR: dinero-cli not found at $CLI_BIN${NC}"
    echo "Run: cmake --build build --target dinero-cli"
    exit 1
fi
echo -e "${GREEN}✅ Binaries found${NC}"
echo "   dinerod: $(ls -lh $DAEMON_BIN | awk '{print $5}')"
echo "   dinero-cli: $(ls -lh $CLI_BIN | awk '{print $5}')"
echo ""

# Step 2: Build checksum
echo -e "${YELLOW}[2/6]${NC} Generating checksums..."
DAEMON_SHA256=$(shasum -a 256 "$DAEMON_BIN" | awk '{print $1}')
CLI_SHA256=$(shasum -a 256 "$CLI_BIN" | awk '{print $1}')
echo -e "${GREEN}✅ Checksums generated${NC}"
echo "   dinerod:    $DAEMON_SHA256"
echo "   dinero-cli: $CLI_SHA256"
echo ""

# Step 3: Deploy to each node
echo -e "${YELLOW}[3/6]${NC} Deploying to production nodes..."
for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Deploying to $SERVER${NC}"

    # Backup existing binaries
    echo "  - Creating backup..."
    ssh "$SERVER" "cd $PATH && cp -f dinerod dinerod.backup.$(date +%Y%m%d_%H%M%S) 2>/dev/null || true"
    ssh "$SERVER" "cd $PATH && cp -f dinero-cli dinero-cli.backup.$(date +%Y%m%d_%H%M%S) 2>/dev/null || true"

    # Copy new binaries
    echo "  - Uploading dinerod..."
    scp -q "$DAEMON_BIN" "$SERVER:$PATH/dinerod.new"

    echo "  - Uploading dinero-cli..."
    scp -q "$CLI_BIN" "$SERVER:$PATH/dinero-cli.new"

    # Make executable
    ssh "$SERVER" "chmod +x $PATH/dinerod.new $PATH/dinero-cli.new"

    echo -e "${GREEN}  ✅ Files uploaded to $SERVER${NC}"
done
echo ""

# Step 4: Stop daemons
echo -e "${YELLOW}[4/6]${NC} Stopping daemons..."
for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Stopping daemon on $SERVER${NC}"

    # Try systemd first, then fallback to pkill
    ssh "$SERVER" "sudo systemctl stop dinerod 2>/dev/null || pkill dinerod || true"
    sleep 2

    echo -e "${GREEN}  ✅ Daemon stopped on $SERVER${NC}"
done
echo ""

# Step 5: Replace binaries
echo -e "${YELLOW}[5/6]${NC} Replacing binaries..."
for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Installing on $SERVER${NC}"
    ssh "$SERVER" "cd $PATH && mv -f dinerod.new dinerod && mv -f dinero-cli.new dinero-cli"

    echo -e "${GREEN}  ✅ Binaries installed on $SERVER${NC}"
done
echo ""

# Step 6: Start daemons
echo -e "${YELLOW}[6/6]${NC} Starting daemons..."
for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Starting daemon on $SERVER${NC}"

    # Try systemd first, then fallback to direct execution
    ssh "$SERVER" "sudo systemctl start dinerod 2>/dev/null || (cd $PATH && nohup ./dinerod --daemon &)"
    sleep 3

    echo -e "${GREEN}  ✅ Daemon started on $SERVER${NC}"
done
echo ""

# Step 7: Verification
echo -e "${YELLOW}[VERIFY]${NC} Checking deployment..."
sleep 5

for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Verifying $SERVER${NC}"

    # Check if process is running
    if ssh "$SERVER" "pgrep -f dinerod > /dev/null"; then
        echo -e "${GREEN}  ✅ Process running${NC}"
    else
        echo -e "${RED}  ❌ Process not found!${NC}"
        continue
    fi

    # Check if serverinfo.json exists
    if ssh "$SERVER" "test -f ~/.dinero/serverinfo.json"; then
        echo -e "${GREEN}  ✅ serverinfo.json created${NC}"

        # Display node_id
        NODE_ID=$(ssh "$SERVER" "cat ~/.dinero/serverinfo.json | grep -o '\"node_id\":\"[^\"]*\"' | cut -d'\"' -f4")
        if [ -n "$NODE_ID" ]; then
            echo -e "${GREEN}  ✅ Node ID: $NODE_ID${NC}"
        fi

        # Check for signature
        if ssh "$SERVER" "grep -q signature ~/.dinero/serverinfo.json"; then
            echo -e "${GREEN}  ✅ Signature present${NC}"
        else
            echo -e "${YELLOW}  ⚠️  No signature found${NC}"
        fi
    else
        echo -e "${YELLOW}  ⚠️  serverinfo.json not yet created (wait 30s)${NC}"
    fi

    echo ""
done

echo -e "${GREEN}╔══════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  Deployment Complete!                        ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}Next steps:${NC}"
echo "1. Wait 30 seconds for serverinfo.json to be created"
echo "2. Test /serverinfo endpoint:"
echo "   curl http://173.249.195.59:21999/serverinfo"
echo "3. Register nodes with registry:"
echo "   cd registry && python3 dinero_registry_extended.py"
echo ""
```

### 🧪 Pre-Deployment Testing

Test the binaries locally before deploying:

```bash
# Start test daemon
rm -rf /tmp/prod-test
mkdir -p /tmp/prod-test
./build/src/dinerod --regtest --rpcport=22999 --datadir=/tmp/prod-test --daemon

# Wait for startup
sleep 5

# Test new features
echo "Testing node identity..."
cat /tmp/prod-test/node_identity.dat | hexdump -C | head -3

echo "Testing serverinfo..."
cat /tmp/prod-test/serverinfo.json | python3 -m json.tool

echo "Testing HTTP endpoint..."
curl -s http://127.0.0.1:22999/serverinfo | python3 -m json.tool

# Cleanup
./build/src/dinero-cli -rpcport=22999 -datadir=/tmp/prod-test stop
```

### 📡 Post-Deployment Verification

Run these commands after deployment:

```bash
# Check Virginia node
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json | python3 -m json.tool"

# Test HTTP endpoint
curl http://173.249.195.59:21999/serverinfo

# Check node identity
ssh root@173.249.195.59 "ls -lah ~/.dinero/node_identity.dat"

# View daemon logs
ssh root@173.249.195.59 "tail -50 ~/.dinero/debug.log | grep -E 'NodeIdentity|ServerInfo'"
```

### 🔍 Troubleshooting

#### Issue: "node_identity.dat not created"
```bash
# Check permissions
ssh root@173.249.195.59 "ls -la ~/.dinero/"

# Check logs
ssh root@173.249.195.59 "grep NodeIdentity ~/.dinero/debug.log"
```

#### Issue: "/serverinfo endpoint returns 404"
```bash
# Check if HTTP RPC is enabled
ssh root@173.249.195.59 "grep rpc ~/.dinero/dinero.conf"

# Should have:
# server=1
# rpcuser=...
# rpcpassword=...
```

#### Issue: "serverinfo.json has no signature"
```bash
# Check if node_identity.dat exists
ssh root@173.249.195.59 "ls -la ~/.dinero/node_identity.dat"

# Restart daemon
ssh root@173.249.195.59 "sudo systemctl restart dinerod"

# Wait 30s and check again
sleep 30
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json | grep signature"
```

### 🔄 Rollback Procedure

If something goes wrong:

```bash
# For each node:
ssh root@173.249.195.59 "sudo systemctl stop dinerod"
ssh root@173.249.195.59 "cd /opt/dinero && cp -f dinerod.backup.* dinerod"
ssh root@173.249.195.59 "sudo systemctl start dinerod"
```

### 📋 Deployment Checklist

- [ ] Build fresh binaries locally
- [ ] Test binaries in regtest mode
- [ ] Backup production binaries on all servers
- [ ] Stop all daemons
- [ ] Deploy new binaries
- [ ] Start all daemons
- [ ] Verify node_identity.dat created
- [ ] Verify serverinfo.json has signatures
- [ ] Test HTTP /serverinfo endpoint
- [ ] Check daemon logs for errors
- [ ] Verify nodes are syncing
- [ ] Register nodes with global registry
- [ ] Monitor for 1 hour

### 🎯 Expected Results

After successful deployment, each node should have:

1. **node_identity.dat** (32 bytes, mode 0600)
   ```bash
   $ ls -la ~/.dinero/node_identity.dat
   -rw------- 1 root root 32 Nov 4 04:00 node_identity.dat
   ```

2. **serverinfo.json with signature**
   ```json
   {
     "node_id": "3cb7355b75d14eb772e49d382c13407c7776836c",
     "node_pubkey": "02565ec96d34f2b5d1eb0ce15b6c2fac252c0a8046b84a3a7ed282ebfba81de1f0",
     "signature": "3045022100...",
     "network": "mainnet",
     "features": ["websocket", "multiasset", "bridge", "contracts"]
   }
   ```

3. **Working /serverinfo endpoint**
   ```bash
   $ curl http://173.249.195.59:21999/serverinfo
   {"node_id":"3cb7...","signature":"3045..."}
   ```

4. **Auto-refresh logs**
   ```
   2025-11-04 04:10:00 [ServerInfo] Auto-refreshed serverinfo.json
   ```

### 📞 Support

If you encounter issues:
1. Check `~/.dinero/debug.log` on the affected server
2. Verify CMakeLists.txt includes `node_identity.cpp`
3. Ensure you built with latest code
4. Check file permissions on node_identity.dat

---

**Last Updated:** November 4, 2025
**Target Servers:** Virginia (173.249.195.59), California (172.93.160.131)
**Critical Files:** dinerod, dinero-cli, node_identity.dat, serverinfo.json
