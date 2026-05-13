# Dinero Cross-Platform Deployment Workflow

## 🎯 Architecture Overview

**Problem**: Mac ARM64 binaries don't run on Linux x86-64 servers.

**Solution**: Use Mac as source of truth, let Linux servers compile natively.

```
┌─────────────────────────────────────────────────────────────┐
│                    Mac (ARM64 - Source)                     │
│  • Write & test code                                        │
│  • Git commit & push                                        │
└──────────────────┬──────────────────────────────────────────┘
                   │ git push virginia main
                   ▼
┌─────────────────────────────────────────────────────────────┐
│           Virginia (x86-64) - Bare Git Repo Hub             │
│  ~/repos/dinero.git (bare)                                  │
└───────┬──────────────────────────────────────────┬──────────┘
        │ git pull                                  │ git pull
        ▼                                           ▼
┌──────────────────────────┐       ┌──────────────────────────┐
│  Virginia (x86-64) Node  │       │ California (x86-64) Node │
│  ~/DineroCoin            │       │  ~/DineroCoin            │
│  • Native Linux build    │       │  • Native Linux build    │
│  • Run dinerod           │       │  • Run dinerod           │
└──────────────────────────┘       └──────────────────────────┘
```

## 📋 One-Time Setup

### Step 1: Set up bare repo on Virginia

```bash
./setup_remote_repos.sh
```

This creates `~/repos/dinero.git` on Virginia and adds it as a remote.

### Step 2: Push initial code

```bash
# Commit current work
git add .
git commit -m "Initial mainnet deployment with consensus fixes"

# Push to Virginia hub
git push virginia main
```

### Step 3: Clone on Virginia node

```bash
ssh root@173.249.195.59
cd ~
git clone ~/repos/dinero.git DineroCoin
cd DineroCoin
./linux_build.sh
```

### Step 4: Clone on California node

```bash
ssh root@172.93.160.131
cd ~
git clone ssh://root@173.249.195.59/~/repos/dinero.git DineroCoin
cd DineroCoin
./linux_build.sh
```

## 🔄 Regular Update Workflow

### Quick update (no restart)

```bash
# On Mac: commit & push
git add .
git commit -m "Fix difficulty adjustment"
git push virginia main

# Deploy to both servers (builds but doesn't restart)
./deploy_all.sh
```

### Update with daemon restart

```bash
# On Mac
git push virginia main

# Deploy + restart daemons
./deploy_all.sh --restart
```

### Manual update on single server

```bash
ssh root@173.249.195.59
cd ~/DineroCoin
git pull
./linux_build.sh
```

## 🛠 Available Scripts

| Script | Purpose | Where to run |
|--------|---------|--------------|
| `setup_remote_repos.sh` | One-time: create bare repo + add remote | Mac |
| `linux_build.sh` | Native Linux x86-64 build | Linux servers |
| `deploy_all.sh` | Update code + build on all servers | Mac |
| `deploy_all.sh --restart` | Update + rebuild + restart daemons | Mac |

## 🔐 Security Notes

- **No GitHub**: Everything stays on your private servers
- **SSH authentication**: Uses your existing SSH keys
- **No secrets in repo**: `.gitignore` excludes wallet.db, .cookie, private keys
- **Bare repo access**: Only accessible from your servers

## 🎛 Server Configuration

### Virginia (173.249.195.59)
- Role: Primary node + Git hub
- Paths:
  - Bare repo: `~/repos/dinero.git`
  - Working copy: `~/DineroCoin`
  - Data: `~/dinero-data`
- Daemon: `~/DineroCoin/build/bin/dinerod -daemon -datadir=~/dinero-data`

### California (172.93.160.131)
- Role: Secondary node
- Paths:
  - Working copy: `~/DineroCoin`
  - Data: `~/dinero-data`
- Daemon: `~/DineroCoin/build/bin/dinerod -daemon -datadir=~/dinero-data`

## 📊 Build Differences

| Platform | Arch | Compiler | Target |
|----------|------|----------|--------|
| Mac | ARM64 | Apple Clang | Development + testing |
| Virginia | x86-64 | GCC/Clang | Production mainnet node |
| California | x86-64 | GCC/Clang | Production mainnet node |

## 🧪 Testing Workflow

1. **Mac**: Full development + unit tests
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
   cmake --build build -j8
   ./build/bin/test_suite
   ```

2. **Linux staging**: Deploy to one server first
   ```bash
   ./deploy_all.sh  # Test on both nodes
   ```

3. **Production**: Use `--restart` to apply changes
   ```bash
   ./deploy_all.sh --restart
   ```

## 🚨 Emergency Rollback

If a deployment breaks something:

```bash
# On affected server
cd ~/DineroCoin
git log --oneline -10  # Find good commit
git reset --hard <good-commit-hash>
./linux_build.sh
pkill -9 dinerod
./build/bin/dinerod -daemon -datadir=~/dinero-data
```

## ✅ Advantages of This Setup

- ✅ **No architecture conflicts** - each machine builds native
- ✅ **Private Git hosting** - no third-party services
- ✅ **Atomic updates** - git ensures consistency
- ✅ **Simple rollback** - git history preserved
- ✅ **Automated deployment** - one script updates all nodes
- ✅ **Mac remains dev machine** - no cross-compilation complexity

## 🔮 Future: CI/CD (Optional)

If you want automated builds:

```bash
# On Virginia, add a post-receive hook:
cat > ~/repos/dinero.git/hooks/post-receive << 'EOF'
#!/bin/bash
cd ~/DineroCoin
git pull
./linux_build.sh
systemctl restart dinerod
EOF
chmod +x ~/repos/dinero.git/hooks/post-receive
```

Then `git push virginia main` will automatically rebuild + restart!

