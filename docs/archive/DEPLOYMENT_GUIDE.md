# 🚀 DineroCoin Incremental Deployment Guide

## Overview

No more painful full rebuilds! This guide shows you how to deploy only changed files to your servers and do incremental builds (1-2 minutes instead of 10-15 minutes).

---

## Quick Start

### Option 1: Full Deployment (sync + build)

```bash
cd ~/Documents/DineroCoin
./deploy.sh
```

**What it does:**
- ✅ Syncs only changed files to both servers via rsync
- ✅ Triggers incremental build (only rebuilds changed files)
- ✅ Takes ~2-5 minutes instead of 10-15 minutes

---

### Option 2: Quick Sync (no build)

If you only changed a few files and want to control the build manually:

```bash
cd ~/Documents/DineroCoin
./quick-deploy.sh
```

Then rebuild on servers:
```bash
# California
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 'cd /root/DineroCoin/build-linux && make -j$(nproc)'

# Virginia
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 'cd /root/DineroCoin/build-linux && make -j$(nproc)'
```

---

## How It Works

### Incremental Builds

CMake's `make` system automatically tracks dependencies. When you run `make` in the build directory:

1. **Changed .cpp files** → Only those files get recompiled
2. **Changed .h files** → Only files that include them get recompiled
3. **Unchanged files** → Skipped entirely
4. **Final linking** → Quick if most object files unchanged

**Example:**
- You change `src/wallet/wallet_manager.cpp`
- Only `wallet_manager.cpp` gets recompiled (~10 seconds)
- Binary gets relinked (~5 seconds)
- **Total time: ~15 seconds instead of 10 minutes!**

### rsync Magic

`rsync` only transfers files that have changed:

```bash
rsync -avz --delete \
    --exclude='build*/' \
    --exclude='.git/' \
    --exclude='data/' \
    . root@server:/root/DineroCoin/
```

**What it excludes:**
- Build artifacts (*.o, binaries)
- Git history
- Blockchain data
- Temporary files

**Result:** Typically transfers < 1 MB even for large changesets

---

## Common Workflows

### Workflow 1: Small Code Change

You edited 1-2 .cpp files:

```bash
./deploy.sh
```

**Time:** ~1-2 minutes total

---

### Workflow 2: Header File Change

You edited a commonly-used header file:

```bash
./deploy.sh
```

**Time:** ~3-5 minutes (more files to recompile)

---

### Workflow 3: CMakeLists.txt Change

You modified build configuration:

```bash
# Sync files
./quick-deploy.sh

# Clean rebuild on servers (needed for CMakeLists.txt changes)
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 'cd /root/DineroCoin && rm -rf build-linux && ./build-server.sh'
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 'cd /root/DineroCoin && rm -rf build-linux && ./build-server.sh'
```

**Time:** ~10-15 minutes (full rebuild needed)

---

### Workflow 4: Deploy and Restart Daemons

```bash
# 1. Deploy code
./deploy.sh

# 2. Restart California daemon
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 '
    pkill -9 dinerod dinero-miner
    cp /root/DineroCoin/build-linux/dinerod /usr/local/bin/
    cp /root/DineroCoin/build-linux/dinero-miner /usr/local/bin/
    nohup /usr/local/bin/dinerod > /var/log/dinerod.log 2>&1 &
'

# 3. Restart Virginia daemon
ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 '
    pkill -9 dinerod dinero-miner
    cp /root/DineroCoin/build-linux/dinerod /usr/local/bin/
    cp /root/DineroCoin/build-linux/dinero-miner /usr/local/bin/
    nohup /usr/local/bin/dinerod > /var/log/dinerod.log 2>&1 &
'
```

---

## Time Comparisons

| Scenario | Old Way | New Way | Savings |
|----------|---------|---------|---------|
| 1-2 file change | 10-15 min | 1-2 min | **85% faster** |
| Header change | 10-15 min | 3-5 min | **70% faster** |
| Large changeset | 10-15 min | 5-7 min | **50% faster** |
| CMakeLists.txt | 10-15 min | 10-15 min | Same (full rebuild needed) |

---

## Troubleshooting

### Problem: "rsync: command not found"

```bash
# On Mac, install rsync
brew install rsync
```

### Problem: Incremental build fails

Sometimes the build system gets confused. Do a clean rebuild:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 'cd /root/DineroCoin && rm -rf build-linux && ./build-server.sh'
```

### Problem: Permission denied (SSH key)

Make sure you're using the correct key:

```bash
ls -la ~/.ssh/dinero_deployment_2025
```

If missing, you need to regenerate and add to servers.

### Problem: Files not syncing

Check if you're in the right directory:

```bash
pwd
# Should show: /Users/haydarevich/Documents/DineroCoin

ls build-server.sh
# Should exist
```

---

## Advanced: Sync Only Specific Files

If you only changed one file:

```bash
rsync -avz \
    -e "ssh -i ~/.ssh/dinero_deployment_2025" \
    src/wallet/wallet_manager.cpp \
    root@172.93.160.131:/root/DineroCoin/src/wallet/

# Then rebuild
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 'cd /root/DineroCoin/build-linux && make -j$(nproc)'
```

---

## Benefits Summary

✅ **Faster deployments** - 1-2 minutes instead of 10-15 minutes
✅ **Less bandwidth** - Only changed files transferred
✅ **Safer** - Incremental builds catch errors faster
✅ **Easier** - Single command deployment
✅ **No more pain in neck** - Actually enjoyable!

---

## Next Steps

1. Try a small change and run `./deploy.sh`
2. Watch it complete in 1-2 minutes
3. Never do full rebuilds again (unless needed)

**Happy deploying!** 🎉
