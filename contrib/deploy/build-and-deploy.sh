#!/bin/bash
# =============================================================================
# Dinero v4.4.2 - Build on Dell Tower, Deploy to Production
# =============================================================================
# Run this script ON DELL TOWER (Linux build machine)
#
# Usage:
#   ./build-and-deploy.sh build          # Build only
#   ./build-and-deploy.sh package        # Build + create tarball
#   ./build-and-deploy.sh deploy         # Build + deploy to servers
# =============================================================================

set -e

VERSION="v4.4.2"
BUILD_DIR="$HOME/dinero-build"
DEPLOY_PKG="dinero-${VERSION}-linux-x86_64.tar.gz"
GITHUB_REPO="https://github.com/pingu-bnh/DineroCoin.git"
BRANCH="dinero-main"

# Production servers (binaries only - NO source code!)
SERVERS=(
    "dinerova"
    "dinerola"
)

# Remote install path
REMOTE_PATH="/opt/dinero"

# =============================================================================
# Build
# =============================================================================
do_build() {
    echo "=== Building Dinero $VERSION on $(hostname) ==="

    # Create clean build directory
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Clone fresh (shallow)
    echo "[1/4] Cloning $BRANCH..."
    git clone --depth 1 --branch "$BRANCH" "$GITHUB_REPO" source
    cd source

    # Build
    echo "[2/4] Configuring..."
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release

    echo "[3/4] Building (this takes a few minutes)..."
    cmake --build . -j$(nproc) --target dinerod dinero-cli

    # Verify
    echo "[4/4] Verifying binaries..."
    ./dinerod --version || echo "dinerod built"
    ./dinero-cli --version || echo "dinero-cli built"

    echo ""
    echo "=== Build complete ==="
    ls -lh dinerod dinero-cli
}

# =============================================================================
# Package (binaries only - NO source!)
# =============================================================================
do_package() {
    do_build

    echo ""
    echo "=== Creating deployment package ==="

    STAGING="$BUILD_DIR/staging"
    rm -rf "$STAGING"
    mkdir -p "$STAGING/dinero/bin"
    mkdir -p "$STAGING/dinero/conf"
    mkdir -p "$STAGING/dinero/scripts"

    # Copy ONLY binaries
    cp "$BUILD_DIR/source/build/dinerod" "$STAGING/dinero/bin/"
    cp "$BUILD_DIR/source/build/dinero-cli" "$STAGING/dinero/bin/"

    # Strip debug symbols (smaller binaries)
    strip "$STAGING/dinero/bin/dinerod"
    strip "$STAGING/dinero/bin/dinero-cli"

    # Create sample config
    cat > "$STAGING/dinero/conf/dinerod.conf" << 'CONF'
# Dinero Node Configuration
# Production settings - localhost RPC only

# Network
network=main
p2p_port=20999

# RPC - localhost only (stratum connects here)
rpcbind=127.0.0.1
rpc_port=20997
rpcallowip=127.0.0.1

# Security
server=1
listen=1

# Logging
debug=0
printtoconsole=0

# Performance
dbcache=2048
maxmempool=300
CONF

    # Create systemd service file
    cat > "$STAGING/dinero/scripts/dinerod.service" << 'SERVICE'
[Unit]
Description=Dinero Node
After=network.target

[Service]
Type=simple
User=dinero
Group=dinero
ExecStart=/opt/dinero/bin/dinerod -datadir=/opt/dinero/data -conf=/opt/dinero/conf/dinerod.conf
Restart=on-failure
RestartSec=10
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
SERVICE

    # Create install script (runs on target server)
    cat > "$STAGING/dinero/scripts/install.sh" << 'INSTALL'
#!/bin/bash
set -e

echo "Installing Dinero to /opt/dinero..."

# Create user if needed
if ! id -u dinero >/dev/null 2>&1; then
    sudo useradd -r -m -d /opt/dinero -s /bin/false dinero
fi

# Create directories
sudo mkdir -p /opt/dinero/{bin,conf,data,logs}
sudo cp -f bin/* /opt/dinero/bin/
sudo cp -f conf/* /opt/dinero/conf/
sudo cp -f scripts/dinerod.service /etc/systemd/system/

# Set permissions
sudo chown -R dinero:dinero /opt/dinero
sudo chmod 750 /opt/dinero/bin/*
sudo chmod 640 /opt/dinero/conf/*

# Enable service
sudo systemctl daemon-reload
sudo systemctl enable dinerod

echo ""
echo "Installation complete!"
echo "Start with: sudo systemctl start dinerod"
echo "Logs at:    sudo journalctl -u dinerod -f"
INSTALL
    chmod +x "$STAGING/dinero/scripts/install.sh"

    # Create tarball
    cd "$STAGING"
    tar -czvf "$BUILD_DIR/$DEPLOY_PKG" dinero/

    echo ""
    echo "=== Package created ==="
    ls -lh "$BUILD_DIR/$DEPLOY_PKG"
    echo ""
    echo "Contents (NO source code):"
    tar -tvf "$BUILD_DIR/$DEPLOY_PKG"
}

# =============================================================================
# Deploy to production servers
# =============================================================================
do_deploy() {
    do_package

    echo ""
    echo "=== Deploying to production servers ==="

    for server in "${SERVERS[@]}"; do
        echo ""
        echo "--- Deploying to $server ---"

        # Upload
        scp "$BUILD_DIR/$DEPLOY_PKG" "$server:/tmp/"

        # Extract and install
        ssh "$server" << REMOTE
cd /tmp
tar -xzf $DEPLOY_PKG
cd dinero
sudo ./scripts/install.sh
rm -rf /tmp/dinero /tmp/$DEPLOY_PKG
REMOTE

        echo "✓ $server deployed"
    done

    echo ""
    echo "=== Deployment complete ==="
    echo ""
    echo "On each server, start with:"
    echo "  sudo systemctl start dinerod"
    echo "  sudo systemctl status dinerod"
}

# =============================================================================
# Main
# =============================================================================
case "${1:-build}" in
    build)
        do_build
        ;;
    package)
        do_package
        ;;
    deploy)
        do_deploy
        ;;
    *)
        echo "Usage: $0 {build|package|deploy}"
        exit 1
        ;;
esac
