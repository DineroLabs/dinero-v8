#!/usr/bin/env bash
set -euo pipefail

# Start Dinero daemon with proper configuration
./build/bin/dinerod -daemon -rpcport=20998 -port=20999 -datadir=./data
