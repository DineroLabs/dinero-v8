#!/usr/bin/env bash
# Build complete Linux binaries (dinerodlinux + dinero-miner-linux) using Docker

set -e

echo "🐧 Building Dinero Linux Binaries in Docker"
echo "============================================"
echo ""

# Clean previous builds
rm -rf build-linux
mkdir -p build-linux

# Build using Ubuntu 22.04 container (x86_64 for Linux servers)
docker run --rm \
  --platform linux/amd64 \
  -v "$(pwd)":/workspace \
  -w /workspace \
  ubuntu:22.04 \
  bash -c '
    set -e
    
    echo "📦 Installing build dependencies..."
    apt-get update -qq
    apt-get install -y -qq \
      build-essential \
      cmake \
      git \
      libssl-dev \
      libsqlite3-dev \
      libjsoncpp-dev \
      pkg-config \
      autoconf \
      automake \
      libtool
    
    echo ""
    echo "🔨 Building secp256k1 (static)..."
    cd secp256k1
    ./autogen.sh
    ./configure --enable-module-recovery --disable-shared --enable-static
    make -j$(nproc)
    make install
    cd ..
    
    echo ""
    echo "🔨 Configuring CMake (static linking)..."
    cmake -S . -B build-linux \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_CXX_COMPILER=g++ \
      -DCMAKE_PREFIX_PATH=/usr/local \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"
    
    echo ""
    echo "⚙️  Building dinerodlinux..."
    cmake --build build-linux -j$(nproc) --target dinerod
    
    echo ""
    echo "⚙️  Building dinero-miner-linux..."
    cmake --build build-linux -j$(nproc) --target dinero-miner
    
    echo ""
    echo "✅ Build complete!"
  '

# Rename binaries
echo ""
echo "📝 Renaming binaries..."
mv build-linux/dinerod build-linux/dinerodlinux
mv build-linux/dinero-miner build-linux/dinero-miner-linux

# Verify binaries
echo ""
echo "🔍 Verifying binaries..."
file build-linux/dinerodlinux
file build-linux/dinero-miner-linux

echo ""
echo "✅ Linux binaries ready:"
ls -lh build-linux/dinerodlinux build-linux/dinero-miner-linux

echo ""
echo "📦 Binaries location:"
echo "   - build-linux/dinerodlinux"
echo "   - build-linux/dinero-miner-linux"
