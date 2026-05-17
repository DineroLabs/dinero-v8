# ✅ Dinero Private Deployment Checklist

**Current Status**: 
- Branch: `feat/sqlite-raii`
- Existing remote: GitHub (origin)
- New setup: Private deployment via Virginia server

---

## 🎯 Execute These Steps (30 minutes total)

### ☐ Step 1: Set up Virginia as Git hub (2 min)

```bash
cd ~/Documents/DineroCoin
./setup_remote_repos.sh
```

**What this does**:
- Creates bare repo at `root@173.249.195.59:~/repos/dinero.git`
- Adds remote called `virginia` (separate from GitHub)

---

### ☐ Step 2: Commit and push current changes (1 min)

```bash
# Commit recent consensus fixes
git add .
git commit -m "Consensus + ExplorerDB mainnet fixes"

# Push to Virginia (private server, NOT GitHub)
git push virginia feat/sqlite-raii
```

**Note**: You can still push to GitHub separately if you want:
```bash
git push origin feat/sqlite-raii  # Optional: public repo
```

---

### ☐ Step 3: Clone and build on Virginia (5 min)

```bash
ssh root@173.249.195.59

# Clone from the bare repo
cd ~
git clone ~/repos/dinero.git DineroCoin
cd DineroCoin

# Build native Linux x86-64 binaries
./linux_build.sh

# Test the build
./build/bin/dinerod --version

# Start daemon
mkdir -p ~/dinero-data
./build/bin/dinerod -daemon \
    -datadir=~/dinero-data \
    -rpcport=20998 \
    -port=20999 \
    -rpcallowip=127.0.0.1 \
    -rpcuser=dinerouser \
    -rpcpassword=YOUR_SECURE_PASSWORD

# Wait 5 seconds then check status
sleep 5
./build/bin/dinero-cli -rpcport=20998 getblockchaininfo

# Exit SSH
exit
```

---

### ☐ Step 4: Clone and build on California (5 min)

```bash
ssh root@172.93.160.131

# Clone from Virginia's bare repo (over SSH)
cd ~
git clone ssh://root@173.249.195.59/~/repos/dinero.git DineroCoin
cd DineroCoin

# Build native Linux x86-64 binaries
./linux_build.sh

# Start daemon
mkdir -p ~/dinero-data
./build/bin/dinerod -daemon \
    -datadir=~/dinero-data \
    -rpcport=20998 \
    -port=20999 \
    -rpcallowip=127.0.0.1 \
    -rpcuser=dinerouser \
    -rpcpassword=YOUR_SECURE_PASSWORD

# Wait 5 seconds then check status
sleep 5
./build/bin/dinero-cli -rpcport=20998 getblockchaininfo

# Exit SSH
exit
```

---

### ☐ Step 5: Test automated deployment (2 min)

```bash
# From your Mac, test the deployment script
cd ~/Documents/DineroCoin

# Make a trivial change (e.g., add a comment somewhere)
echo "# Test deployment" >> README.md
git add README.md
git commit -m "Test: verify deployment automation"
git push virginia feat/sqlite-raii

# Deploy to both servers WITHOUT restarting
./deploy_all.sh

# Check output - should see:
# ✅ Virginia deployment complete
# ✅ California deployment complete
```

---

### ☐ Step 6: Verify both nodes are syncing (5 min)

```bash
# Check Virginia
ssh root@173.249.195.59 \
  '~/DineroCoin/build/bin/dinero-cli -rpcport=20998 getblockcount'

# Check California
ssh root@172.93.160.131 \
  '~/DineroCoin/build/bin/dinero-cli -rpcport=20998 getblockcount'

# Both should return the same (or very close) block height
```

---

### ☐ Step 7: Set up systemd services (optional, 10 min)

**On Virginia:**
```bash
ssh root@173.249.195.59

sudo tee /etc/systemd/system/dinerod.service > /dev/null <<EOF
[Unit]
Description=Dinero Cryptocurrency Daemon
After=network.target

[Service]
Type=forking
User=root
WorkingDirectory=/root/DineroCoin
ExecStart=/root/DineroCoin/build/bin/dinerod -daemon -datadir=/root/dinero-data -rpcport=20998
ExecStop=/root/DineroCoin/build/bin/dinero-cli -rpcport=20998 stop
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable dinerod
sudo systemctl start dinerod
sudo systemctl status dinerod

exit
```

**Repeat on California** (same commands)

---

## 🎉 Success Criteria

After completing all steps, you should have:

- ✅ **Virginia**: Running dinerod, syncing blockchain
- ✅ **California**: Running dinerod, syncing blockchain
- ✅ **Mac**: Can deploy to both with `./deploy_all.sh`
- ✅ **Both nodes**: Respond to `dinero-cli getblockchaininfo`
- ✅ **Both nodes**: Same or very close block height

---

## 🔄 From Now On: Daily Workflow

```bash
# 1. Make changes on Mac
vim src/consensus/pow.cpp

# 2. Test locally
cmake --build build --target dinerod -j8

# 3. Commit & push
git add .
git commit -m "Improve difficulty adjustment"
git push virginia feat/sqlite-raii

# 4. Deploy to production (auto-builds on both servers)
./deploy_all.sh --restart
```

**Done in 30 seconds!** ⚡

---

## 🔗 Git Remote Strategy

You now have **two remotes**:

| Remote | URL | Purpose |
|--------|-----|---------|
| `origin` | GitHub | Optional: public source / backup |
| `virginia` | SSH to 173.249.195.59 | Production deployment |

**Workflow**:
```bash
git push origin feat/sqlite-raii      # Public GitHub (optional)
git push virginia feat/sqlite-raii    # Private production (required)
./deploy_all.sh --restart             # Auto-deploy to both nodes
```

---

## 📚 Documentation

- **Quick Start**: `DEPLOYMENT_QUICKSTART.md` - Common commands
- **Full Guide**: `DEPLOYMENT_WORKFLOW.md` - Complete architecture details
- **This File**: `DEPLOYMENT_CHECKLIST.md` - One-time setup steps

---

## 🚨 Troubleshooting

### If `./setup_remote_repos.sh` fails:

```bash
# Manually set up bare repo
ssh root@173.249.195.59 "mkdir -p ~/repos && cd ~/repos && git init --bare dinero.git"

# Add remote on Mac
git remote add virginia ssh://root@173.249.195.59/~/repos/dinero.git
```

### If build fails on Linux:

```bash
# Install dependencies
ssh root@173.249.195.59
apt update && apt install -y build-essential cmake libssl-dev git

cd ~/DineroCoin
./linux_build.sh
```

### If daemon won't start:

```bash
# Check if already running
ps aux | grep dinerod

# Kill old process
pkill -9 dinerod

# Check logs
tail -f ~/dinero-data/debug.log

# Start with verbose output
~/DineroCoin/build/bin/dinerod -daemon -datadir=~/dinero-data -debug=1
```

---

**Ready?** Start with Step 1! 🚀
