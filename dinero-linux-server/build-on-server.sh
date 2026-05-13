#!/bin/bash
# Build Dinero directly on the Linux server

set -e

echo "🔨 Building Dinero on Linux Server..."
echo "📊 Server: Ubuntu 22.04.5 LTS (1GB RAM, 1 CPU)"
echo ""

# Update system
echo "📦 Updating system..."
sudo apt update

# Install build dependencies
echo "🔧 Installing build dependencies..."
sudo apt install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libboost-all-dev \
    libevent-dev \
    libminiupnpc-dev \
    libzmq3-dev \
    qtbase5-dev \
    qttools5-dev-tools \
    libqt5websockets5-dev \
    libqrencode-dev

# Clone or copy source code
if [ ! -d "DineroCoin" ]; then
    echo "📥 Cloning Dinero source code..."
    git clone https://github.com/your-repo/DineroCoin.git
    cd DineroCoin
else
    echo "📁 Using existing source code..."
    cd DineroCoin
fi

# Build with memory-optimized settings
echo "🔨 Building Dinero (optimized for 1GB RAM)..."
mkdir -p build
cd build

# Use less parallel jobs to avoid OOM
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DENABLE_WS=ON \
    -DENABLE_TERMINAL=OFF

# Build with limited parallelism (1 core server)
make -j1 dinerod dinero-cli

# Strip binaries to save space
strip bin/dinerod bin/dinero-cli

# Copy to system location
echo "📋 Installing binaries..."
sudo cp bin/dinerod /opt/dinero/
sudo cp bin/dinero-cli /opt/dinero/
sudo chown dinero:dinero /opt/dinero/dinerod /opt/dinero/dinero-cli
sudo chmod +x /opt/dinero/dinerod /opt/dinero/dinero-cli

echo ""
echo "✅ Build complete!"
echo "📊 Binary sizes:"
ls -lh /opt/dinero/dinerod /opt/dinero/dinero-cli

echo ""
echo "🚀 Start the node:"
echo "sudo systemctl start dinerod"
