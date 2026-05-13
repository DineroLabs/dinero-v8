#!/bin/bash
# Deploy vnext with all 181 RPC methods to Linux production servers

set -e

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
LOCAL_SOURCE="/Users/haydarevich/Documents/DineroCoin"

# Production nodes
NODES=(
    "root@173.249.195.59"  # Virginia
    "root@172.93.160.131"  # California
)

echo "=========================================="
echo "Dinero vnext Deployment - All 181 Methods"
echo "=========================================="
echo ""
echo "This will deploy the latest daemon with:"
echo "  - Node Identity System (secp256k1)"
echo "  - HTTP /serverinfo endpoint"
echo "  - Multi-Asset Escrow"
echo "  - ALL 181 RPC methods (157 original + 24 migrated)"
echo ""

for NODE in "${NODES[@]}"; do
    echo "=========================================="
    echo "Deploying to: $NODE"
    echo "=========================================="

    # 1. Create source tarball
    echo "[1/8] Creating source tarball..."
    cd "$LOCAL_SOURCE"
    tar czf /tmp/dinero-vnext-sources.tar.gz \
        src/ \
        include/ \
        test/ \
        third_party/ \
        CMakeLists.txt \
        config/ \
        --exclude='*.o' \
        --exclude='build/' \
        --exclude='.git/' \
        2>/dev/null

    echo "[2/8] Uploading sources to $NODE..."
    /usr/bin/scp -i "$SSH_KEY" /tmp/dinero-vnext-sources.tar.gz "$NODE:/root/dinero-vnext-sources.tar.gz"

    echo "[3/8] Stopping old daemon..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "systemctl stop dinerod 2>/dev/null || pkill -9 dinerod || true"

    echo "[4/8] Extracting sources..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "cd /root && rm -rf DineroCoin-vnext && mkdir -p DineroCoin-vnext && cd DineroCoin-vnext && tar xzf /root/dinero-vnext-sources.tar.gz"

    echo "[5/8] Installing dependencies..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "apt-get update -qq && apt-get install -y -qq cmake g++ libssl-dev libboost-all-dev libusb-1.0-0-dev pkg-config"

    echo "[6/8] Building daemon (this may take 5-10 minutes)..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "cd /root/DineroCoin-vnext && mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j\$(nproc) dinerod dinero-cli"

    echo "[7/8] Installing binaries..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "cd /root/DineroCoin-vnext/build && cp dinerod dinero-cli /usr/local/bin/ && chmod +x /usr/local/bin/dinerod /usr/local/bin/dinero-cli"

    echo "[8/8] Starting daemon..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "systemctl start dinerod && sleep 5"

    echo ""
    echo "✅ $NODE deployment complete!"
    echo ""

    # Verify deployment
    echo "Verifying deployment..."
    /usr/bin/ssh -i "$SSH_KEY" "$NODE" "systemctl status dinerod | head -15"
    echo ""

done

echo ""
echo "=========================================="
echo "✅ All servers deployed successfully!"
echo "=========================================="
echo ""
echo "Next steps:"
echo "1. Test Node Identity: ssh root@173.249.195.59 'cat ~/.dinero/node_identity.dat | wc -c'"
echo "2. Test /serverinfo: curl http://173.249.195.59:20998/serverinfo"
echo "3. Test RPC: ssh root@173.249.195.59 'dinero-cli getblockchaininfo'"
echo ""
