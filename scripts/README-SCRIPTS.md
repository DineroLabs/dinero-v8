# Dinero Utility Scripts

Quick reference for managing your Dinero wallet and daemon.

## 🚀 Quick Start

### Start the GUI Wallet
```bash
cd /Users/haydarevich/Documents/DineroCoin
./scripts/start-gui.sh
```
This script will:
- Create default config with seed nodes if needed
- Clean up any stale processes
- Start the GUI wallet

### Clean Up Stuck Processes
If you ever have multiple daemons running or the wallet won't start:
```bash
cd /Users/haydarevich/Documents/DineroCoin
./scripts/cleanup-daemons.sh
```
This will:
- Kill all running dinerod processes
- Kill the GUI if running
- Remove lock files
- Verify cleanup was successful

## 📝 Manual Commands

### Start GUI manually:
```bash
cd /Users/haydarevich/Documents/DineroCoin && ./build/gui/dinero-qt
```

### Start daemon only (no GUI):
```bash
cd /Users/haydarevich/Documents/DineroCoin && ./build/dinerod -datadir=~/.dinero
```

### Check if daemon is running:
```bash
pgrep -fl dinerod
```

### Kill all daemons manually:
```bash
killall -9 dinerod && rm -f ~/.dinero/.lock
```

## 🔧 Configuration

Config location: `~/.dinero/dinero.conf`

Default seed nodes:
- rpc.dinero-coin.com (172.93.160.131:20999)
- rpc1.dinero-coin.com (173.249.195.59:20999)

## ⚠️ Troubleshooting

### "Another daemon instance is already running"
Run the cleanup script:
```bash
./scripts/cleanup-daemons.sh
```

### "No connections" in GUI
1. Make sure config file exists: `cat ~/.dinero/dinero.conf`
2. Restart the GUI: `./scripts/start-gui.sh`
3. Wait 30 seconds for connections to establish

### Multiple daemons running
This happens if you start the daemon multiple times. Always use the cleanup script first:
```bash
./scripts/cleanup-daemons.sh
./scripts/start-gui.sh
```
