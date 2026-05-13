#!/bin/bash
# One-line deployment for web console
cd /root && \
wget YOUR_FILE_HOST/dinero-wallet-deployment-20251004.tar.gz && \
tar -xzf dinero-wallet-deployment-20251004.tar.gz && \
cd DineroCoin && \
rm -rf build && mkdir build && cd build && \
cmake .. -DCMAKE_BUILD_TYPE=Release && \
make -j$(nproc) dinerod && \
pkill -9 dinerod || true && \
sleep 2 && \
nohup ./dinerod -datadir=/root/dinero-data -rpcport=20998 -port=20999 -testnet > /root/dinerod.log 2>&1 & \
sleep 3 && \
tail -20 /root/dinerod.log
