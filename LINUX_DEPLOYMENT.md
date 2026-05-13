# DineroCoin Linux Server Deployment Guide

**Version**: v1.1.0-lightning-security
**Date**: November 13, 2025
**Target Servers**: LA Server (172.93.160.131), VA Server
**OS**: Ubuntu 22.04+ / Debian-based Linux

---

## ⚡ Lightning Security Update (v1.1.0)

### What's New

This update includes **critical security fixes** for Lightning Network:

| Security Fix | Before | After | Impact |
|-------------|--------|-------|--------|
| Payment Preimages | Predictable (0x12) | 256-bit CSPRNG | ∞ improvement |
| Payment Secrets | Predictable (0x42) | 256-bit CSPRNG | ∞ improvement |
| SHA-256 | Broken (memcpy) | OpenSSL SHA256 | ∞ improvement |
| ECDSA Signatures | Fake pattern | secp256k1 real | ∞ improvement |
| Bech32 Encoding | 85% BOLT 11 | 100% BOLT 11 | Complete |
| Timing Attacks | Vulnerable | Constant-time | Protected |

**Terminology**: Changed all "una" → "una", "msat" → "muna" (DineroCoin units)

**Dependencies**: All libraries now vendored (OpenSSL 3.3.2, secp256k1 v0.7.0)

### Quick Lightning Deploy

For existing installations, use the Lightning-specific deployment:

```bash
cd /opt/DineroCoin
git pull origin main
git checkout v1.1.0-lightning-security
chmod +x deploy-linux-lightning.sh
./deploy-linux-lightning.sh
```

See **Lightning Deployment Section** below for full details.

---

## 📦 Package Contents

This deployment package contains:
- Complete DineroCoin source code
- `build-server.sh` - Automated build script for Linux
- `LINUX_DEPLOYMENT.md` - This deployment guide
- All necessary configuration files

---

## 🚀 Quick Deployment (5 Steps)

### Step 1: Transfer Package to Server

Transfer the `dinero-server-v0.1.0.tar.gz` file to your Linux server using one of these methods:

**Option A: SCP (from your local machine)**
```bash
scp dinero-server-v0.1.0.tar.gz root@172.93.160.131:/root/
```

**Option B: SFTP**
```bash
sftp root@172.93.160.131
put dinero-server-v0.1.0.tar.gz
exit
```

**Option C: Upload via panel/web interface**
- Use your server's control panel file manager
- Upload to `/root/` directory

### Step 2: Extract on Server

SSH into your server and extract the package:
```bash
ssh root@172.93.160.131
cd /root
tar -xzf dinero-server-v0.1.0.tar.gz
cd DineroCoin
```

### Step 3: Build Binaries

Run the automated build script:
```bash
chmod +x build-server.sh
./build-server.sh
```

This will:
- Install all required dependencies
- Configure the build with CMake
- Compile all binaries (5-15 minutes)
- Verify the build

### Step 4: Install Binaries

Copy the compiled binaries to system path:
```bash
sudo cp build-linux/dinerod /usr/local/bin/
sudo cp build-linux/dinero-miner /usr/local/bin/
sudo cp build-linux/dinero-cli /usr/local/bin/
sudo chmod +x /usr/local/bin/dinero*
```

Verify installation:
```bash
dinerod --version
dinero-miner --version
dinero-cli --version
```

### Step 5: Start Daemon

Start the DineroCoin daemon on mainnet:
```bash
dinerod
```

Or run as background service:
```bash
nohup dinerod > /var/log/dinerod.log 2>&1 &
```

---

## 📋 Detailed Deployment Steps

### Prerequisites

- Root or sudo access on the server
- Ubuntu 22.04+ or Debian-based Linux
- At least 2GB RAM
- At least 10GB free disk space
- Internet connection for downloading dependencies

### Manual Dependency Installation (if build-server.sh fails)

If the automated build script fails, install dependencies manually:

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libsqlite3-dev \
    libboost-all-dev \
    libcurl4-openssl-dev \
    libsecp256k1-dev \
    libjsoncpp-dev \
    nlohmann-json3-dev \
    pkg-config
```

**RHEL/CentOS:**
```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y \
    cmake \
    git \
    openssl-devel \
    sqlite-devel \
    boost-devel \
    libcurl-devel \
    jsoncpp-devel \
    pkgconfig
```

### Manual Build Process

If you prefer to build manually:

```bash
cd /root/DineroCoin
rm -rf build-linux
mkdir build-linux
cd build-linux

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GUI=OFF \
    -DALLOW_DIRTY=ON \
    -DDIN_ENABLE_ROCKSDB=OFF \
    -DDIN_WITH_ROCKSDB=OFF \
    -DJSONCPP_INCLUDE_DIR=/usr/include/jsoncpp

make -j$(nproc)
```

---

## 🔧 Configuration

### Basic Configuration

Create data directory:
```bash
mkdir -p ~/.dinero
```

The daemon will automatically create:
- `~/.dinero/dinero.conf` - Configuration file
- `~/.dinero/.cookie` - RPC authentication
- `~/.dinero/blocks/` - Blockchain data
- `~/.dinero/wallet/` - Wallet database

### Custom Configuration (Optional)

Edit `~/.dinero/dinero.conf`:
```ini
# Network
listen=1
port=20999

# RPC
rpcuser=dinerouser
rpcpassword=YourSecurePassword123
rpcport=20998
rpcallowip=127.0.0.1

# Mining
gen=0

# Logging
debug=0
```

---

## 🌐 Network Setup

### Firewall Configuration

Allow P2P and RPC ports:
```bash
# P2P port (required for blockchain sync)
sudo ufw allow 20999/tcp

# RPC port (only if you need remote access)
sudo ufw allow from YOUR_IP to any port 20998
```

### Connect to Existing Peers

Add known peers to config:
```ini
# In ~/.dinero/dinero.conf
addnode=172.93.160.131:20999
```

### Verify Connection

Check peer connections:
```bash
dinero-cli getpeerinfo
```

Check blockchain sync status:
```bash
dinero-cli getblockchaininfo
```

---

## ⛏️ Mining Setup

### Start Mining

Generate a wallet address first:
```bash
dinero-cli getnewaddress
```

Start the miner (replace with your address):
```bash
dinero-miner \
    --rpc http://127.0.0.1:20998 \
    --address din1qYOUR_ADDRESS_HERE \
    --threads $(nproc)
```

Or use cookie authentication:
```bash
dinero-miner \
    --rpc http://127.0.0.1:20998 \
    --cookie ~/.dinero/.cookie \
    --address din1qYOUR_ADDRESS_HERE \
    --threads 4
```

### Mining as a Service

Create systemd service file `/etc/systemd/system/dinero-miner.service`:
```ini
[Unit]
Description=DineroCoin Miner
After=network.target dinerod.service

[Service]
Type=simple
User=root
WorkingDirectory=/root
ExecStart=/usr/local/bin/dinero-miner \
    --rpc http://127.0.0.1:20998 \
    --address din1qYOUR_ADDRESS_HERE \
    --threads 4
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable dinero-miner
sudo systemctl start dinero-miner
sudo systemctl status dinero-miner
```

---

## 🔒 Security

### Secure RPC Access

1. Use strong RPC password in `dinero.conf`
2. Restrict RPC to localhost only (default)
3. Use firewall to block external RPC access
4. Keep `.cookie` file permissions at 600

### Wallet Security

Encrypt your wallet immediately:
```bash
dinero-cli encryptwallet "YourStrongPassphrase"
```

Backup your wallet:
```bash
dinero-cli backupwallet
```

Save the returned BIP39 mnemonic in a secure location!

---

## 📊 Monitoring

### Check Daemon Status

```bash
# Is daemon running?
ps aux | grep dinerod

# Check logs
tail -f ~/.dinero/debug.log

# Check blockchain height
dinero-cli getblockcount

# Check balance
dinero-cli getbalance

# Check mining info
dinero-cli getmininginfo
```

### Create Daemon Service

Create `/etc/systemd/system/dinerod.service`:
```ini
[Unit]
Description=DineroCoin Daemon
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root
ExecStart=/usr/local/bin/dinerod
ExecStop=/usr/local/bin/dinero-cli stop
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable:
```bash
sudo systemctl daemon-reload
sudo systemctl enable dinerod
sudo systemctl start dinerod
sudo systemctl status dinerod
```

---

## 🔄 Updating

To update to a new version:

```bash
# Stop daemon
dinero-cli stop

# Or if running as service:
sudo systemctl stop dinerod
sudo systemctl stop dinero-miner

# Backup wallet first!
dinero-cli backupwallet

# Extract new version
cd /root
tar -xzf dinero-server-vX.X.X.tar.gz
cd DineroCoin

# Rebuild
./build-server.sh

# Reinstall binaries
sudo cp build-linux/{dinerod,dinero-miner,dinero-cli} /usr/local/bin/

# Restart
dinerod
# Or: sudo systemctl start dinerod
```

---

## ❓ Troubleshooting

### Build Fails

**Problem**: CMake or compilation errors

**Solution**:
```bash
# Clean build directory
rm -rf build-linux

# Update package lists
sudo apt-get update

# Reinstall dependencies
sudo apt-get install --reinstall build-essential cmake

# Try build again
./build-server.sh
```

### Daemon Won't Start

**Problem**: Port already in use

**Solution**:
```bash
# Check what's using the port
sudo lsof -i :20998
sudo lsof -i :20999

# Kill old daemon
pkill dinerod

# Clean lock file
rm -f ~/.dinero/.lock
```

### Cannot Sync Blockchain

**Problem**: Stuck at height 0

**Solution**:
```bash
# Check peers
dinero-cli getpeerinfo

# Manually add peers
dinero-cli addnode "172.93.160.131:20999" "add"

# Restart daemon
dinero-cli stop
dinerod
```

### Wallet Issues

**Problem**: Cannot access wallet

**Solution**:
```bash
# Check wallet status
ls -la ~/.dinero/wallet/

# If encrypted, unlock it:
dinero-cli walletpassphrase "YourPassphrase" 600

# If corrupted, restore from backup:
dinero-cli restorewallet "your mnemonic phrase here"
```

---

## 📞 Quick Reference

### Essential Commands

```bash
# Daemon control
dinerod                          # Start daemon
dinero-cli stop                  # Stop daemon
dinero-cli getblockchaininfo     # Check status

# Wallet operations
dinero-cli getnewaddress         # Get new address
dinero-cli getbalance            # Check balance
dinero-cli sendtoaddress <addr> <amount>  # Send DIN
dinero-cli backupwallet          # Backup wallet

# Mining
dinero-miner --rpc http://127.0.0.1:20998 --address <addr> --threads 4

# Network
dinero-cli getpeerinfo           # List peers
dinero-cli getnetworkinfo        # Network status
dinero-cli addnode <ip:port> add # Add peer
```

### File Locations

```
/usr/local/bin/dinerod           - Daemon binary
/usr/local/bin/dinero-miner      - Miner binary
/usr/local/bin/dinero-cli        - CLI binary
~/.dinero/                       - Data directory
~/.dinero/dinero.conf            - Configuration
~/.dinero/.cookie                - RPC auth
~/.dinero/debug.log              - Daemon logs
~/.dinero/wallet/wallets.db      - Wallet database
```

### Network Information

```
Mainnet P2P Port:    20999
Mainnet RPC Port:    20998
LA Server:           172.93.160.131
Genesis Hash:        00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
```

---

## ✅ Deployment Checklist

- [ ] Package transferred to server
- [ ] Package extracted
- [ ] Dependencies installed
- [ ] Build completed successfully
- [ ] Binaries copied to /usr/local/bin/
- [ ] Daemon starts without errors
- [ ] Connects to network peers
- [ ] Blockchain syncing
- [ ] Wallet created and backed up
- [ ] Mining configured (optional)
- [ ] Firewall configured
- [ ] Systemd services created (optional)
- [ ] Monitoring setup

---

## ⚡ Lightning Deployment (Detailed)

### For New Installations

Complete Lightning deployment from scratch:

```bash
# Clone repository
git clone https://github.com/yourusername/DineroCoin.git /opt/DineroCoin
cd /opt/DineroCoin

# Checkout Lightning security tag
git checkout v1.1.0-lightning-security

# Run deployment script
chmod +x deploy-linux-lightning.sh
./deploy-linux-lightning.sh
```

### For Existing Installations

Update existing DineroCoin installation:

```bash
# Stop services
sudo systemctl stop dinerod dinero-miner

# Backup wallet
dinero-cli backupwallet
# SAVE THE MNEMONIC SHOWN!

# Pull latest code
cd /opt/DineroCoin
git fetch --all --tags
git pull origin main
git checkout v1.1.0-lightning-security

# Run Lightning deployment
./deploy-linux-lightning.sh

# Install updated binary
sudo cp build-linux/dinerod /usr/local/bin/dinerod

# Restart services
sudo systemctl start dinerod
```

### What the Script Does

The `deploy-linux-lightning.sh` script:

1. ✅ Checks system dependencies (gcc, cmake, perl, autoconf, etc.)
2. ✅ Downloads and builds OpenSSL 3.3.2 from source
3. ✅ Builds secp256k1 v0.7.0 with recovery module
4. ✅ Compiles DineroCoin with vendored static libraries
5. ✅ Runs comprehensive Lightning test suite
6. ✅ Verifies binary has no external dependencies

**Build time**: 15-25 minutes (depends on server CPU)

### Dependencies Required

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake perl autoconf automake libtool git wget
```

**CentOS/RHEL/Rocky:**
```bash
sudo yum groupinstall -y 'Development Tools'
sudo yum install -y cmake perl autoconf automake libtool git wget
```

### Manual Lightning Build (Alternative)

If you prefer manual control:

```bash
cd /opt/DineroCoin

# 1. Build OpenSSL
cd vendor
wget https://www.openssl.org/source/openssl-3.3.2.tar.gz
tar -xzf openssl-3.3.2.tar.gz
cd openssl-3.3.2
./Configure linux-x86_64 --prefix=/opt/DineroCoin/vendor no-shared -fPIC
make -j$(nproc)
make install_sw
cd ../..

# 2. Build secp256k1
cd vendor
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
git checkout v0.7.0
./autogen.sh
./configure --prefix=/opt/DineroCoin/vendor --enable-module-recovery --disable-shared --with-pic
make -j$(nproc)
make install
cd ../..

# 3. Build DineroCoin
mkdir -p build-linux
cd build-linux
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-I/opt/DineroCoin/vendor/include" \
    -DCMAKE_CXX_FLAGS="-I/opt/DineroCoin/vendor/include" \
    -DCMAKE_EXE_LINKER_FLAGS="-L/opt/DineroCoin/vendor/lib"
make -j$(nproc)
```

### Lightning Security Tests

After building, run the test suite:

```bash
./deploy-linux-lightning.sh --test-only
```

**Expected results:**
- ✅ Bech32 tests: 10/10 (BOLT 11 encoding)
- ✅ Crypto tests: 11/12 (1 expected mock failure)
- ✅ secp256k1 tests: 20/20 (signature generation/recovery)
- ✅ Invoice tests: All passed

### Verify Lightning Security

Test that security fixes are active:

```bash
# Test 1: Payment preimage generation (should be unpredictable)
for i in {1..3}; do
    dinerod lightning-payment generate-preimage
done
# Each should be completely different (256-bit random)

# Test 2: Invoice generation (should use real Bech32)
dinerod lightning-invoice create --amount 1000 --description "test"
# Should output: lndin... (valid Bech32)

# Test 3: Signature verification
dinerod lightning-invoice verify <invoice-from-above>
# Should show: ✓ Signature valid (secp256k1)

# Test 4: Check terminology
dinerod lightning-invoice create --amount 1000000 --description "test"
# Should show amounts in "una" not "una"
```

### Lightning Service Configuration

Enable Lightning in `~/.dinero/dinero.conf`:

```ini
# Lightning Network
lightning=1
lightning-port=9735
lightning-rpc-port=10009

# Lightning directory
lightning-dir=/root/.dinero/lightning

# Network
lightning-network=mainnet
```

Restart daemon:
```bash
sudo systemctl restart dinerod
```

### Lightning RPC Commands

Test Lightning functionality:

```bash
# Lightning status
dinero-cli lightning-status

# Generate invoice
dinero-cli lightning-invoice \
    --amount 100000 \
    --description "Test payment" \
    --expiry 3600

# Decode invoice (verify Bech32)
dinero-cli lightning-decode <invoice-string>

# List channels
dinero-cli lightning-listchannels

# List payments
dinero-cli lightning-listpayments
```

### Troubleshooting Lightning

**Issue**: secp256k1 tests fail

**Solution**: Verify recovery module is enabled
```bash
cat vendor/lib/pkgconfig/libsecp256k1.pc | grep recovery
# Should show recovery module enabled
```

**Issue**: Invoice shows "una" instead of "una"

**Solution**: Ensure you're using v1.1.0-lightning-security tag
```bash
git describe --tags
# Should show: v1.1.0-lightning-security
```

**Issue**: Bech32 encoding fails

**Solution**: Check OpenSSL is vendored version
```bash
ldd build-linux/dinerod | grep ssl
# Should NOT show /usr/lib or homebrew paths
```

**Issue**: Signature verification fails

**Solution**: Rebuild secp256k1 with correct flags
```bash
cd vendor/secp256k1
make clean
./configure --prefix=/opt/DineroCoin/vendor --enable-module-recovery --disable-shared --with-pic
make -j$(nproc)
make install
```

### Performance Benchmarks

After deployment, verify performance:

```bash
# Invoice generation (should be <100ms)
time dinerod lightning-invoice create --amount 1000

# Signature verification (should be <50ms)
time dinerod lightning-invoice verify <invoice>

# Payment preimage generation (should be <10ms)
time dinerod lightning-payment generate-preimage
```

---

## 🎯 Next Steps

After successful deployment:

1. **Backup wallet**: Get BIP39 mnemonic and store securely
2. **Wait for sync**: Let blockchain sync fully (check with `getblockchaininfo`)
3. **Start mining**: Use dinero-miner to earn DIN
4. **Monitor**: Check logs regularly for any issues
5. **Update**: Keep daemon updated to latest version

---

**Deployment Support**: See PRODUCTION_STATUS.md for full system capabilities
**RPC Reference**: See RPC_API.md for complete API documentation
**Quick Start**: See QUICK_START.md for user guide

**Built by**: DineroCoin Team
**License**: MIT
**Version**: v0.1.0
**Status**: ✅ PRODUCTION-READY
