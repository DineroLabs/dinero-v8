#!/bin/bash
set -e  # Exit on any error

# Set PATH
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
NODES=(
    "root@173.249.195.59:/usr/local/bin"    # Virginia
    "root@172.93.160.131:/usr/local/bin"    # California (Los Angeles)
)

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
BUILD_DIR="$HOME/Documents/DineroCoin/build"
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
    TIMESTAMP=$(/bin/date +%Y%m%d_%H%M%S)
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "cd $PATH && cp -f dinerod dinerod.backup.$TIMESTAMP 2>/dev/null || true"
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "cd $PATH && cp -f dinero-cli dinero-cli.backup.$TIMESTAMP 2>/dev/null || true"

    # Copy new binaries
    echo "  - Uploading dinerod..."
    /usr/bin/scp -i "$SSH_KEY" -q "$DAEMON_BIN" "$SERVER:$PATH/dinerod.new"

    echo "  - Uploading dinero-cli..."
    /usr/bin/scp -i "$SSH_KEY" -q "$CLI_BIN" "$SERVER:$PATH/dinero-cli.new"

    # Make executable
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "chmod +x $PATH/dinerod.new $PATH/dinero-cli.new"

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
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "sudo systemctl stop dinerod 2>/dev/null || pkill dinerod || true"
    /bin/sleep 2

    echo -e "${GREEN}  ✅ Daemon stopped on $SERVER${NC}"
done
echo ""

# Step 5: Replace binaries
echo -e "${YELLOW}[5/6]${NC} Replacing binaries..."
for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Installing on $SERVER${NC}"
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "cd $PATH && mv -f dinerod.new dinerod && mv -f dinero-cli.new dinero-cli"

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
    /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "sudo systemctl start dinerod 2>/dev/null || (cd $PATH && nohup ./dinerod --daemon &)"
    /bin/sleep 3

    echo -e "${GREEN}  ✅ Daemon started on $SERVER${NC}"
done
echo ""

# Step 7: Verification
echo -e "${YELLOW}[VERIFY]${NC} Checking deployment..."
/bin/sleep 5

for NODE in "${NODES[@]}"; do
    SERVER="${NODE%%:*}"
    PATH="${NODE##*:}"

    echo -e "${BLUE}→ Verifying $SERVER${NC}"

    # Check if process is running
    if /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "pgrep -f dinerod > /dev/null"; then
        echo -e "${GREEN}  ✅ Process running${NC}"
    else
        echo -e "${RED}  ❌ Process not found!${NC}"
        continue
    fi

    # Check if serverinfo.json exists
    if /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "test -f ~/.dinero/serverinfo.json"; then
        echo -e "${GREEN}  ✅ serverinfo.json created${NC}"

        # Display node_id
        NODE_ID=$(/usr/bin/ssh -i "$SSH_KEY" "$SERVER" "cat ~/.dinero/serverinfo.json | grep -o '\"node_id\":\"[^\"]*\"' | cut -d'\"' -f4")
        if [ -n "$NODE_ID" ]; then
            echo -e "${GREEN}  ✅ Node ID: $NODE_ID${NC}"
        fi

        # Check for signature
        if /usr/bin/ssh -i "$SSH_KEY" "$SERVER" "grep -q signature ~/.dinero/serverinfo.json"; then
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
