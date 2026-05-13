#!/bin/bash
#
# DineroCoin Deployment Script for Dell Precision 7920
# Optimized for: 2×Xeon Silver 4116 (48 threads) + AMD WX 2100
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}════════════════════════════════════════${NC}"
echo -e "${GREEN}DineroCoin Setup - Dell Precision 7920${NC}"
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo ""

# System info
echo -e "${YELLOW}System Information:${NC}"
echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)"
echo "Cores: $(nproc) threads"
echo "Memory: $(free -h | grep Mem | awk '{print $2}')"
echo "GPU: $(lspci | grep -i vga | cut -d: -f3)"
echo "Disk: $(df -h / | tail -1 | awk '{print $4}') available"
echo ""

# Check dependencies
echo -e "${YELLOW}Installing dependencies...${NC}"
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libboost-all-dev \
    libsodium-dev \
    pkg-config \
    autoconf \
    automake \
    libtool \
    perl \
    ocl-icd-opencl-dev \
    mesa-opencl-icd \
    clinfo

echo -e "${GREEN}✓ Dependencies installed${NC}"
echo ""

# AMD GPU OpenCL setup
echo -e "${YELLOW}Configuring AMD OpenCL for WX 2100...${NC}"
sudo apt-get install -y mesa-opencl-icd
clinfo | head -20 || echo "OpenCL info not available yet"
echo ""

# Clone DineroCoin
INSTALL_DIR="$HOME/DineroCoin"
if [ -d "$INSTALL_DIR" ]; then
    echo -e "${YELLOW}DineroCoin directory exists, updating...${NC}"
    cd "$INSTALL_DIR"
    git fetch --all --tags
else
    echo -e "${YELLOW}Cloning DineroCoin...${NC}"
    cd ~
    # Replace with actual repo URL
    echo "Please run: git clone <YOUR_REPO_URL> DineroCoin"
    echo "Then run this script again"
    exit 1
fi

# Checkout Lightning release
echo -e "${YELLOW}Checking out v1.2.0 (Lightning)...${NC}"
git checkout v1.2.0
git submodule update --init --recursive
echo -e "${GREEN}✓ Source code ready${NC}"
echo ""

# Build
echo -e "${YELLOW}Building DineroCoin (optimized for 48 threads)...${NC}"
mkdir -p build-precision
cd build-precision

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIGHTNING=ON \
    -DENABLE_GPU_MINING=ON \
    -DCMAKE_CXX_FLAGS="-march=native -O3"

echo -e "${YELLOW}Compiling with all 48 threads (this will take 5-10 minutes)...${NC}"
make -j48

echo -e "${GREEN}✓ Build complete!${NC}"
echo ""

# Binary info
BINARY="$INSTALL_DIR/build-precision/dinerod"
if [ -f "$BINARY" ]; then
    echo -e "${GREEN}✓ Binary created: $BINARY${NC}"
    ls -lh "$BINARY"
    echo ""
else
    echo -e "${RED}✗ Build failed - binary not found${NC}"
    exit 1
fi

# Create data directory
DATA_DIR="$HOME/dinero_data"
mkdir -p "$DATA_DIR"

# Create optimized config
echo -e "${YELLOW}Creating optimized configuration...${NC}"
cat > "$DATA_DIR/dinero.conf" <<EOF
# DineroCoin Configuration - Dell Precision 7920
# Generated: $(date)

# Network
port=20999
rpcport=20998
rpcuser=dinerorpc
rpcpassword=$(openssl rand -hex 32)
rpcallowip=127.0.0.1

# Performance (48 CPU threads)
maxconnections=200
dbcache=4096

# Mining (CPU + GPU)
gen=1
genproclimit=48        # Use all 48 threads
gpumining=1            # Enable AMD WX 2100

# Lightning Network
lightning=1
lightning-port=9735
lightning-alias=Precision7920Node

# Logging
debug=0
logtimestamps=1
EOF

echo -e "${GREEN}✓ Configuration created at $DATA_DIR/dinero.conf${NC}"
echo ""

# Create systemd service
echo -e "${YELLOW}Creating systemd service...${NC}"
sudo tee /etc/systemd/system/dinerod.service > /dev/null <<EOF
[Unit]
Description=DineroCoin Node
After=network.target

[Service]
Type=forking
User=$USER
Group=$USER
ExecStart=$BINARY -daemon -conf=$DATA_DIR/dinero.conf -datadir=$DATA_DIR
ExecStop=$BINARY -conf=$DATA_DIR/dinero.conf -datadir=$DATA_DIR stop
Restart=on-failure
RestartSec=10
TimeoutStopSec=600

# Performance
LimitNOFILE=65536
CPUQuota=4800%

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
echo -e "${GREEN}✓ Systemd service created${NC}"
echo ""

# Summary
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo -e "${GREEN}Setup Complete!${NC}"
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo ""
echo -e "${BLUE}Hardware Optimization:${NC}"
echo "  • CPU Mining: 48 threads (2×Xeon Silver 4116)"
echo "  • GPU Mining: AMD Radeon Pro WX 2100 (OpenCL)"
echo "  • Memory: 32 GB ECC"
echo "  • Network: Dual Gigabit LAN"
echo ""
echo -e "${BLUE}To start DineroCoin:${NC}"
echo "  sudo systemctl start dinerod"
echo "  sudo systemctl enable dinerod   # Auto-start on boot"
echo ""
echo -e "${BLUE}To check status:${NC}"
echo "  sudo systemctl status dinerod"
echo "  $BINARY -datadir=$DATA_DIR getinfo"
echo "  $BINARY -datadir=$DATA_DIR getmininginfo"
echo ""
echo -e "${BLUE}To view logs:${NC}"
echo "  tail -f $DATA_DIR/debug.log"
echo ""
echo -e "${BLUE}Lightning Network:${NC}"
echo "  $BINARY -datadir=$DATA_DIR lightning-cli getinfo"
echo ""
echo -e "${YELLOW}Note: First sync will take 30-60 minutes depending on blockchain size${NC}"
echo ""
