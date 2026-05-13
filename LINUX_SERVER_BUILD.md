# Linux Server Build Guide

**Date**: 2025-10-30
**Target**: Ubuntu 22.04 LTS / Debian 12 (recommended for servers)
**Architecture**: x86_64 (amd64)

## Overview

This guide covers building Dinero binaries for Linux servers with **fully static linking** for maximum portability.

---

## Quick Build (Docker - Recommended)

The easiest way to build Linux binaries from macOS is using Docker:

### Option 1: Docker Build (No Linux Machine Required!)

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Build using Ubuntu 22.04 container
docker run --rm \
  -v "$PWD:/workspace" \
  -w /workspace \
  --platform linux/amd64 \
  ubuntu:22.04 \
  bash -c '
    export DEBIAN_FRONTEND=noninteractive

    # Install build dependencies
    apt-get update -qq
    apt-get install -y -qq \
      build-essential cmake git \
      libssl-dev libsqlite3-dev \
      libcurl4-openssl-dev \
      pkg-config

    # Create build directory
    rm -rf build-linux
    mkdir -p build-linux
    cd build-linux

    # Configure (vendored dependencies)
    cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DUSE_VENDORED_ROCKSDB=ON \
      -DENABLE_SANITIZERS=OFF \
      -DBUILD_GUI=OFF

    # Build all binaries
    make -j$(nproc) dinerod dinero-cli dinero-miner

    # Strip binaries (reduce size)
    strip dinerod dinero-cli dinero-miner 2>/dev/null || true

    # Show results
    echo ""
    echo "✅ Linux binaries built successfully!"
    echo ""
    ls -lh dinerod dinero-cli dinero-miner
    echo ""
    file dinerod
  '

# Binaries will be in: build-linux/
```

**Output:**
```
build-linux/
├── dinerod         (15-20 MB)
├── dinero-cli      (250-300 KB)
└── dinero-miner    (300-350 KB)
```

---

## Option 2: Native Linux Build

If you have access to a Linux server or VM:

### Install Dependencies (Ubuntu/Debian)

```bash
# Update package lists
sudo apt-get update

# Install build tools
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  pkg-config

# Install libraries (for vendored build, only minimal deps needed)
sudo apt-get install -y \
  libssl-dev \
  libsqlite3-dev \
  libcurl4-openssl-dev

# Optional: for non-vendored build
sudo apt-get install -y \
  libboost-all-dev \
  libjsoncpp-dev
```

### Build

```bash
cd /path/to/DineroCoin

# Create build directory
mkdir -p build-linux
cd build-linux

# Configure with vendored dependencies
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_VENDORED_ROCKSDB=ON \
  -DENABLE_SANITIZERS=OFF \
  -DBUILD_GUI=OFF

# Build all server binaries
make -j$(nproc) dinerod dinero-cli dinero-miner

# Strip debug symbols (reduce size)
strip dinerod dinero-cli dinero-miner

# Verify
./dinerod --version
./dinero-cli --version
./dinero-miner --version
```

---

## Static Linking (Maximum Portability)

To build binaries that work on ANY Linux distribution without dependencies:

### Fully Static Build

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_VENDORED_ROCKSDB=ON \
  -DENABLE_SANITIZERS=OFF \
  -DBUILD_GUI=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
  -DBUILD_SHARED_LIBS=OFF

make -j$(nproc) dinerod dinero-cli dinero-miner
strip dinerod dinero-cli dinero-miner
```

### Verify Static Linking

```bash
# Check dependencies (should only show linux-vdso and libc)
ldd dinerod

# Expected output (minimal dependencies):
linux-vdso.so.1 (0x00007ffd...)
libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0
libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
/lib64/ld-linux-x86-64.so.2
```

---

## Server Deployment

### Create Distribution Package

```bash
# On build machine
cd build-linux

# Create package directory
mkdir -p Dinero-Linux-Server-v0.1.0/bin
mkdir -p Dinero-Linux-Server-v0.1.0/config
mkdir -p Dinero-Linux-Server-v0.1.0/systemd

# Copy binaries
cp dinerod dinero-cli dinero-miner Dinero-Linux-Server-v0.1.0/bin/

# Create config template
cat > Dinero-Linux-Server-v0.1.0/config/dinero.conf << 'EOF'
# Dinero Server Configuration

# RPC Settings
rpcuser=dinerouser
rpcpassword=CHANGE_THIS_PASSWORD
rpcport=20998
rpcbind=127.0.0.1
rpcallowip=127.0.0.1

# Network
port=20999
maxconnections=125

# Server optimizations
daemon=1
server=1
listen=1

# Database
dbcache=2048
maxmempool=500

# Logging
debug=0
EOF

# Create systemd service
cat > Dinero-Linux-Server-v0.1.0/systemd/dinerod.service << 'EOF'
[Unit]
Description=Dinero Cryptocurrency Daemon
After=network.target

[Service]
Type=forking
User=dinero
Group=dinero
WorkingDirectory=/opt/dinero
ExecStart=/opt/dinero/bin/dinerod -daemon -conf=/etc/dinero/dinero.conf -datadir=/var/lib/dinero
ExecStop=/opt/dinero/bin/dinero-cli -conf=/etc/dinero/dinero.conf stop
Restart=on-failure
RestartSec=5s
TimeoutStopSec=120s

# Security
PrivateTmp=true
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF

# Create installation script
cat > Dinero-Linux-Server-v0.1.0/install.sh << 'EOF'
#!/bin/bash
set -e

echo "Installing Dinero Server..."

# Create dinero user
if ! id -u dinero > /dev/null 2>&1; then
    useradd -r -m -d /var/lib/dinero -s /bin/bash dinero
    echo "✓ Created dinero user"
fi

# Install binaries
install -m 755 bin/dinerod /usr/local/bin/
install -m 755 bin/dinero-cli /usr/local/bin/
install -m 755 bin/dinero-miner /usr/local/bin/
echo "✓ Installed binaries to /usr/local/bin/"

# Create directories
mkdir -p /etc/dinero
mkdir -p /var/lib/dinero
chown dinero:dinero /var/lib/dinero
echo "✓ Created data directories"

# Install config
if [ ! -f /etc/dinero/dinero.conf ]; then
    install -m 600 -o dinero -g dinero config/dinero.conf /etc/dinero/
    echo "✓ Installed configuration"
    echo "⚠️  IMPORTANT: Edit /etc/dinero/dinero.conf and change the RPC password!"
else
    echo "ℹ️  Config exists at /etc/dinero/dinero.conf (not overwritten)"
fi

# Install systemd service
install -m 644 systemd/dinerod.service /etc/systemd/system/
systemctl daemon-reload
echo "✓ Installed systemd service"

echo ""
echo "Installation complete!"
echo ""
echo "Next steps:"
echo "1. Edit /etc/dinero/dinero.conf and set RPC password"
echo "2. Enable service: systemctl enable dinerod"
echo "3. Start service:  systemctl start dinerod"
echo "4. Check status:   systemctl status dinerod"
echo "5. View logs:      journalctl -u dinerod -f"
EOF

chmod +x Dinero-Linux-Server-v0.1.0/install.sh

# Create README
cat > Dinero-Linux-Server-v0.1.0/README.md << 'EOF'
# Dinero Linux Server Package

## Quick Install

```bash
sudo ./install.sh
```

## Manual Configuration

1. Edit config:
   ```bash
   sudo nano /etc/dinero/dinero.conf
   ```

2. Change RPC password!

3. Enable and start:
   ```bash
   sudo systemctl enable dinerod
   sudo systemctl start dinerod
   ```

## Usage

```bash
# Check status
systemctl status dinerod

# View logs
journalctl -u dinerod -f

# Use CLI
dinero-cli getblockcount
dinero-cli getbalance

# Mine
dinero-miner --address=din1q... --threads=8
```

## Firewall

```bash
# Allow P2P port
sudo ufw allow 20999/tcp

# RPC port (only if needed remotely - NOT recommended)
# sudo ufw allow 20998/tcp
```
EOF

# Create tarball
tar czf Dinero-Linux-Server-v0.1.0.tar.gz Dinero-Linux-Server-v0.1.0/

echo ""
echo "✅ Package created: Dinero-Linux-Server-v0.1.0.tar.gz"
echo ""
ls -lh Dinero-Linux-Server-v0.1.0.tar.gz
```

---

## Server Installation

### 1. Upload to Server

```bash
# From build machine
scp Dinero-Linux-Server-v0.1.0.tar.gz user@server:/tmp/

# On server
cd /tmp
tar xzf Dinero-Linux-Server-v0.1.0.tar.gz
cd Dinero-Linux-Server-v0.1.0
```

### 2. Run Installation Script

```bash
sudo ./install.sh
```

### 3. Configure

```bash
# Edit config
sudo nano /etc/dinero/dinero.conf

# Change RPC password!
rpcpassword=YOUR_SECURE_PASSWORD_HERE
```

### 4. Start Service

```bash
# Enable auto-start on boot
sudo systemctl enable dinerod

# Start now
sudo systemctl start dinerod

# Check status
sudo systemctl status dinerod

# View logs
sudo journalctl -u dinerod -f
```

### 5. Verify Running

```bash
# Check blockchain sync
dinero-cli getblockcount

# Check connections
dinero-cli getconnectioncount

# Check wallet
dinero-cli getbalance
```

---

## Server Optimization

### For High-Performance Servers

Edit `/etc/dinero/dinero.conf`:

```conf
# Increase database cache (4GB)
dbcache=4096

# Increase mempool (1GB)
maxmempool=1000

# More connections
maxconnections=250

# Enable indexes for faster lookups
txindex=1
addressindex=1

# Thread count for validation
par=8  # Adjust to CPU core count
```

### Firewall Configuration

```bash
# Ubuntu/Debian with UFW
sudo ufw allow 20999/tcp comment 'Dinero P2P'

# CentOS/RHEL with firewalld
sudo firewall-cmd --permanent --add-port=20999/tcp
sudo firewall-cmd --reload

# Check open ports
sudo ss -tlnp | grep dinero
```

---

## Mining on Server

### Start CPU Miner

```bash
# Get mining address
ADDR=$(dinero-cli getnewaddress)

# Start miner (8 threads)
nohup dinero-miner \
  --address=$ADDR \
  --threads=8 \
  --cookiefile=/var/lib/dinero/.cookie \
  > /var/log/dinero-miner.log 2>&1 &

# Check mining
tail -f /var/log/dinero-miner.log
```

### Miner Systemd Service (Optional)

```bash
sudo nano /etc/systemd/system/dinero-miner.service
```

```ini
[Unit]
Description=Dinero CPU Miner
After=dinerod.service
Requires=dinerod.service

[Service]
Type=simple
User=dinero
Group=dinero
ExecStart=/usr/local/bin/dinero-miner \
  --address=din1q2t8jsqdujthgrf7ump4w8pczl00qvjp7a5t24f \
  --threads=8 \
  --cookiefile=/var/lib/dinero/.cookie
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable dinero-miner
sudo systemctl start dinero-miner
```

---

## Monitoring

### System Resource Usage

```bash
# CPU and memory
top -p $(pgrep dinerod)

# Disk usage
df -h /var/lib/dinero

# Network connections
ss -tn | grep :20999 | wc -l
```

### Blockchain Stats

```bash
# Quick status script
cat > /usr/local/bin/dinero-status << 'EOF'
#!/bin/bash
echo "=== Dinero Node Status ==="
echo ""
echo "Height:      $(dinero-cli getblockcount)"
echo "Connections: $(dinero-cli getconnectioncount)"
echo "Mempool:     $(dinero-cli getmempoolinfo | grep size | cut -d: -f2 | tr -d ' ,')"
echo "Balance:     $(dinero-cli getbalance) DIN"
echo ""
echo "Service:     $(systemctl is-active dinerod)"
echo "Uptime:      $(systemctl show dinerod -p ActiveEnterTimestamp --value)"
EOF

chmod +x /usr/local/bin/dinero-status

# Run it
dinero-status
```

---

## Backup & Recovery

### Backup Wallet

```bash
# Create backup
sudo -u dinero dinero-cli backupwallet /var/backups/dinero-wallet-$(date +%Y%m%d).db

# Automated daily backup (cron)
sudo crontab -u dinero -e

# Add line:
0 2 * * * /usr/local/bin/dinero-cli backupwallet /var/backups/dinero-wallet-$(date +\%Y\%m\%d).db
```

### Restore Wallet

```bash
sudo systemctl stop dinerod
sudo cp /var/backups/dinero-wallet-20251030.db /var/lib/dinero/wallet/wallets.db
sudo chown dinero:dinero /var/lib/dinero/wallet/wallets.db
sudo systemctl start dinerod
```

---

## Troubleshooting

### Daemon Won't Start

```bash
# Check logs
sudo journalctl -u dinerod -n 50 --no-pager

# Check permissions
ls -la /var/lib/dinero

# Test manually
sudo -u dinero /usr/local/bin/dinerod -conf=/etc/dinero/dinero.conf
```

### Port Already in Use

```bash
# Find what's using port
sudo ss -tlnp | grep :20999

# Kill old process
sudo pkill dinerod
sleep 5
sudo systemctl start dinerod
```

### Database Corruption

```bash
# Backup wallet first!
sudo systemctl stop dinerod
sudo -u dinero dinero-cli backupwallet /tmp/wallet-backup.db

# Remove blockchain data (keeps wallet)
sudo rm -rf /var/lib/dinero/blocks
sudo rm -rf /var/lib/dinero/chaindb

# Restart (will resync)
sudo systemctl start dinerod
```

---

## Production Deployment Checklist

- [ ] Build binaries with vendored dependencies
- [ ] Verify static linking (`ldd dinerod`)
- [ ] Create distribution package
- [ ] Upload to server
- [ ] Run installation script
- [ ] Set strong RPC password
- [ ] Configure firewall (allow port 20999)
- [ ] Enable systemd service
- [ ] Start daemon
- [ ] Verify sync status
- [ ] Setup automated backups
- [ ] Configure monitoring
- [ ] Test miner (optional)
- [ ] Document recovery procedure

---

## Security Best Practices

1. **Firewall**: Only open P2P port (20999), NOT RPC port (20998)
2. **RPC Password**: Use strong random password (32+ chars)
3. **User Permissions**: Run as dedicated `dinero` user
4. **Wallet Encryption**: Encrypt wallet with strong passphrase
5. **Regular Backups**: Daily automated wallet backups
6. **System Updates**: Keep OS and packages updated
7. **Monitoring**: Setup alerts for service failures
8. **SSH Security**: Use key-based auth, disable root login

---

## Quick Reference

```bash
# Service management
sudo systemctl start dinerod
sudo systemctl stop dinerod
sudo systemctl restart dinerod
sudo systemctl status dinerod

# Logs
sudo journalctl -u dinerod -f

# CLI commands
dinero-cli getblockcount
dinero-cli getconnectioncount
dinero-cli getbalance
dinero-cli getnewaddress
dinero-cli sendtoaddress <addr> <amount>

# Mining
dinero-miner --address=<addr> --threads=8 --cookiefile=/var/lib/dinero/.cookie
```

---

**Your Linux server binaries are ready to deploy!** 🐧🚀
