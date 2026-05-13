# Dinero-Qt Wallet - User Guide

**Version**: 1.0  
**Platform**: macOS, Linux, Windows

---

## 🚀 Quick Start (For End Users)

### Step 1: Install Dinero

Download the appropriate package for your system:
- **macOS**: `DineroCoin-Qt.dmg`
- **Windows**: `DineroCoin-Qt-Setup.exe`
- **Linux**: `DineroCoin-Qt.AppImage` or `.deb`/.rpm`

### Step 2: Start the Dinero Daemon

Before opening the GUI, you need to run the blockchain daemon:

**macOS/Linux:**
```bash
# Navigate to installation directory
cd /Applications/Dinero  # or wherever installed

# Start testnet (for testing)
./dinerod --testnet

# OR start mainnet (for real transactions)
./dinerod
```

**Windows:**
```cmd
# Navigate to installation folder
cd "C:\Program Files\Dinero"

# Start testnet
dinerod.exe --testnet

# OR start mainnet
dinerod.exe
```

**Wait for sync**: The daemon will start syncing the blockchain. This may take time on first run.

### Step 3: Open the GUI

**macOS:**
```bash
open /Applications/Dinero/dinero-qt.app
```

**Linux:**
```bash
./dinero-qt
```

**Windows:**
- Double-click `dinero-qt.exe`
- Or use Start Menu shortcut

### Step 4: Create Your Wallet

1. GUI will prompt you to create a new HD wallet
2. Write down your **12-word seed phrase** (keep it safe!)
3. Set a strong password to encrypt your wallet
4. Done! You can now receive and send DIN

---

## 🔧 Default Configuration

The GUI automatically connects to:
- **Testnet**: `localhost:20998` (default)
- **Mainnet**: `localhost:20997`

No additional configuration needed for typical users!

---

## 🌐 Network Selection

### Using Testnet (Recommended for Learning)

**Purpose**: Testing and development without risking real money

**Setup**:
```bash
# Start daemon in testnet mode
dinerod --testnet

# GUI will auto-connect to localhost:20998
```

**Features**:
- Free test coins
- Same functionality as mainnet
- Safe to experiment
- Can be reset

### Using Mainnet (Production)

**Purpose**: Real cryptocurrency transactions

**Setup**:
```bash
# Start daemon in mainnet mode
dinerod

# GUI will auto-connect to localhost:20997
```

**⚠️ Warning**: Mainnet uses real DIN with real value. Secure your seed phrase!

---

## 📁 Data Directories

The wallet stores data in standard locations:

**macOS:**
```
~/Library/Application Support/Dinero/
├── .cookie          (RPC authentication)
├── wallet.dat       (encrypted wallet)
├── blocks/          (blockchain data)
└── chainstate/      (UTXO database)
```

**Linux:**
```
~/.dinero/
├── .cookie
├── wallet.dat
├── blocks/
└── chainstate/
```

**Windows:**
```
%APPDATA%\Dinero\
├── .cookie
├── wallet.dat
├── blocks/
└── chainstate/
```

---

## 🔑 Wallet Security

### Seed Phrase (12 Words)
- **Never share** with anyone
- **Write on paper** and store safely
- **DO NOT** save digitally (no screenshots, no cloud)
- Can restore wallet on any device

### Wallet Password
- Encrypts wallet on disk
- Required for sending transactions
- Can be changed: Settings → Change Password
- Cannot be recovered if forgotten!

### Best Practices
1. ✅ Backup seed phrase immediately
2. ✅ Use strong, unique password
3. ✅ Test wallet recovery before storing large amounts
4. ✅ Lock wallet when not in use
5. ✅ Keep software updated

---

## 💰 Common Tasks

### Receive DIN
1. Go to **Receive** tab
2. Click **New Address**
3. Copy the `din1q...` address
4. Share with sender
5. Transaction appears in **Transactions** tab

### Send DIN
1. Go to **Send** tab
2. Enter recipient's `din1q...` address
3. Enter amount
4. Review fee (auto-calculated)
5. Click **Send Transaction**
6. Confirm with password
7. Wait for confirmations

### Mining (CPU)
1. Go to **Mining** tab
2. Click **Use Wallet Address** (or enter custom address)
3. Set thread count (default: 8)
4. Click **▶️ Start**
5. Monitor hashrate and blocks found
6. Mining rewards appear after 100 confirmations

---

## 🛠️ Troubleshooting

### "Connection Refused" Error

**Cause**: Daemon is not running

**Fix**:
```bash
# Start the daemon first
dinerod --testnet  # or without --testnet for mainnet

# Then open GUI
```

### "Unauthorized" Error

**Cause**: Cookie authentication failed

**Fix**:
1. Close GUI
2. Restart daemon
3. Wait 5 seconds for cookie to generate
4. Reopen GUI

### GUI Won't Open

**macOS specific**:
```bash
# Give execute permission
chmod +x /Applications/Dinero/dinero-qt.app/Contents/MacOS/dinero-qt

# Or right-click → Open (first time only)
```

**Linux specific**:
```bash
chmod +x dinero-qt
./dinero-qt
```

### Sync Taking Forever

**Normal**: First sync downloads entire blockchain (can take hours)

**Check progress**: Overview tab shows sync percentage

**Speed up**:
- Close other applications
- Use wired internet connection
- Wait patiently (especially on HDD)

---

## 🔐 Advanced Configuration

### Custom RPC Server

For advanced users connecting to remote nodes:

**Environment variable**:
```bash
export DINERO_RPC_URL="http://custom-server:20998/"
./dinero-qt
```

**Or edit configuration** (create if doesn't exist):
```
# macOS/Linux: ~/Library/Application Support/Dinero/dinero-qt.conf
# Windows: %APPDATA%\Dinero\dinero-qt.conf

rpc_url=http://custom-server:20998/
```

### Custom Data Directory

```bash
# macOS/Linux
export DINERO_DATA_DIR="/custom/path/to/data"
./dinero-qt

# Windows
set DINERO_DATA_DIR=C:\custom\path\to\data
dinero-qt.exe
```

### Custom Miner Path

```bash
export DINERO_MINER_PATH="/custom/path/to/dinero-miner"
./dinero-qt
```

---

## 📱 Mobile Wallet Compatibility

Your Dinero-Qt wallet is **compatible with mobile wallets**!

### iOS Wallet

1. Export seed phrase: **Wallet** → **Export Seed for Mobile**
2. Install DineroCoin iOS app
3. Choose **Restore Wallet**
4. Enter your 12-word seed phrase
5. ✅ Same wallet, same addresses on both devices!

### Benefits
- Use desktop for mining and advanced features
- Use mobile for quick payments on-the-go
- One seed phrase = Access everywhere

---

## ❓ FAQ

**Q: Where is my seed phrase stored?**  
A: Only in your head! Write it down when creating wallet. Never stored digitally.

**Q: Can I change my seed phrase?**  
A: No. Create new wallet if compromised and transfer funds.

**Q: What if I lose my password?**  
A: Use seed phrase to restore wallet with new password.

**Q: What if I lose my seed phrase?**  
A: **Cannot be recovered**. Your funds are lost forever. Backup is critical!

**Q: How many confirmations are needed?**  
A: 6 confirmations (~60 minutes) for security. Mining rewards need 100 confirmations.

**Q: Can I run multiple wallets?**  
A: Yes, using different data directories:
```bash
dinerod --datadir=~/wallet1 --testnet
# In another terminal:
dinerod --datadir=~/wallet2 --testnet -port=21000 -rpcport=21001
```

**Q: Is my wallet encrypted?**  
A: Yes, with your password. Seed phrase provides additional recovery backup.

**Q: Where can I get testnet coins?**  
A: Mine them (free) or ask in Discord/Telegram for faucet.

---

## 🆘 Getting Help

- **Discord**: https://discord.gg/dinero
- **Telegram**: https://t.me/dinerocoin
- **GitHub Issues**: https://github.com/dinero/dinero/issues
- **Email**: support@dinero-coin.com

---

## 📜 License & Credits

DineroCoin is open source (MIT License)

**Development Team**:
- Core Developers: [Team list]
- GUI Development: [Contributors]
- Community Contributors: [List]

**Acknowledgments**:
- Qt Framework
- Bitcoin Core (inspiration)
- All contributors and testers

---

## 🔄 Updates

**Current Version**: 1.0.0

**Check for updates**:
```bash
./dinerod --version
./dinero-qt --version
```

**Download latest**: https://dinero-coin.com/download

**Changelog**: https://github.com/dinero/dinero/releases

---

**Last Updated**: October 2025  
**Maintainer**: Dinero Development Team
