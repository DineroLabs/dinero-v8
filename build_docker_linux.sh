#!/usr/bin/env bash
set -ueo pipefail

LOGDIR="./build-logs"
mkdir -p "$LOGDIR"
LOGFILE="$LOGDIR/build_$(date +%Y%m%d_%H%M%S).log"

echo "→ Starting Linux build inside Docker…"
echo "  Log: $LOGFILE"

docker run --rm -v "$PWD:/ws" -w /ws --platform linux/amd64 ubuntu:22.04 \
bash -ueo pipefail -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq build-essential cmake ninja-build \
      libssl-dev libboost-all-dev libsqlite3-dev \
      libcurl4-openssl-dev libsecp256k1-dev libjsoncpp-dev \
      nlohmann-json3-dev git pkg-config ccache >/dev/null

  # speed up rebuilds
  export CCACHE_DIR=/root/.ccache
  ccache -M 5G >/dev/null || true

  cmake -E rm -rf build-linux
  cmake -S . -B build-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DALLOW_DIRTY=ON \
    -DDIN_ENABLE_ROCKSDB=OFF -DDIN_WITH_ROCKSDB=OFF \
    -DJSONCPP_INCLUDE_DIR=/usr/include/jsoncpp

  cmake --build build-linux --target dinerod -j $(nproc)
  strip build-linux/bin/dinerod || true
  ls -lh build-linux/bin/dinerod
  file build-linux/bin/dinerod
' | tee "$LOGFILE"

echo
echo "→ Error summary (first 50 matches):"
grep -nE "error:|undefined reference|ld: error|collect2: error" "$LOGFILE" | head -50 || echo "No compiler/linker errors found."
echo
echo "Done. Full log: $LOGFILE"

