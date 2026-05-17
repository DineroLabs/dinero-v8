# 🚀 Dinero Mainnet Launch Checklist

## ✅ **Pre-Launch Validation (COMPLETE)**

### Consensus Economics
- [x] Phase 1: 100 DIN × 200,000 blocks = 20,000,000 DIN
- [x] Phase 2: 50 DIN × 790,000 blocks × 2 (geometric) = 79,000,000 DIN
- [x] Total: 99,000,000 DIN
- [x] Units: 1 DIN = 100,000,000 una
- [x] Halving interval: 790,000 blocks
- [x] Difficulty: 0x2100ffff (Phase 1) → 0x1d00ffff (Phase 2)

### Genesis Block
- [x] Genesis hash: `d6cd82dac8bcb2e2452f696d401be53ff9717ea1419d05e7fa88c98ad5cccdb7`
- [x] Merkle root: `e54b9767be5161e7d6584500c7a987dc5ab48e08c4a1f82dd62d552db028fc6f`
- [x] Timestamp: 1696118400 (2023-10-01 00:00:00 UTC)
- [x] Nonce: 0
- [x] Burned: 100,000 DIN (provably unspendable)
- [x] Message: "Dinero: 100,000 DIN burned at genesis for network security."

### Technical Implementation
- [x] Zero build errors
- [x] Zero warnings
- [x] Zero placeholders
- [x] All values computed from params (no hardcoded strings)
- [x] Real cryptography (secp256k1 + HASH160 + bech32)
- [x] Vendored crypto (no external dependencies)
- [x] Cookie authentication working
- [x] P2P networking tested (multi-node)
- [x] Supply tracking with persistence
- [x] Genesis miner tool included

## 📋 **Launch Preparation**

### 1. Freeze & Tag Release
```bash
# Final commit
git add -A
git commit -m "chore: lock Dinero mainnet consensus (99M DIN, 790K halving)"

# Tag release
git tag -a v1.0.0-mainnet -m "Dinero Mainnet Genesis - IMMUTABLE CONSENSUS"
git push origin v1.0.0-mainnet
```

### 2. Build Release Artifacts
```bash
# macOS arm64
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j8
tar -czf dinero-v1.0.0-macos-arm64.tar.gz -C build-release dinerod genesis_miner

# Generate checksums
shasum -a 256 dinero-v1.0.0-macos-arm64.tar.gz > SHA256SUMS.txt

# Sign (GPG)
gpg --detach-sign --armor SHA256SUMS.txt
```

### 3. Seed Nodes Setup
```bash
# Seed Node 1 (VPS 1)
./dinerod -datadir=/var/lib/dinero \
          -rpcbind=127.0.0.1 \
          -rpcport=20998 \
          -port=20999 \
          -daemon

# Seed Node 2 (VPS 2)
./dinerod -datadir=/var/lib/dinero \
          -rpcbind=127.0.0.1 \
          -rpcport=20998 \
          -port=20999 \
          -addnode=<seed1_ip>:20999 \
          -daemon

# Seed Node 3 (VPS 3)
./dinerod -datadir=/var/lib/dinero \
          -rpcbind=127.0.0.1 \
          -rpcport=20998 \
          -port=20999 \
          -addnode=<seed1_ip>:20999 \
          -addnode=<seed2_ip>:20999 \
          -daemon
```

### 4. Security Hardening
- [ ] All seed nodes: RPC bound to `127.0.0.1` only
- [ ] Firewall: Allow `20999/tcp`, deny `20998/tcp`
- [ ] Cookie files: `0600` permissions
- [ ] Running as non-root user `dinero`
- [ ] SSL/TLS proxy for external access (optional)
- [ ] Rate limiting on P2P connections
- [ ] Monitoring & alerting configured

### 5. Documentation
- [ ] `GENESIS.md` - Genesis block specification
- [ ] `ECONOMICS.md` - Complete economic model
- [ ] `BUILD.md` - Build instructions for all platforms
- [ ] `README.md` - Quick start guide
- [ ] `API.md` - RPC method documentation

## 🎯 **Launch Timeline**

### T-24 Hours
- [ ] Announce tag `v1.0.0-mainnet`
- [ ] Publish genesis specification
- [ ] Share build instructions
- [ ] List seed node addresses

### T-2 Hours
- [ ] Start all 3+ seed nodes
- [ ] Verify P2P connectivity between seeds
- [ ] Check port forwarding from external

### T-0 (Launch)
- [ ] Publish binaries + SHA256SUMS + signatures
- [ ] Announce on social media / forums
- [ ] Share quick-start guide
- [ ] Open community channels

### T+10 Minutes
- [ ] Monitor peer connections on seeds
- [ ] Verify clients connecting successfully
- [ ] Check genesis propagation

### T+1 Hour
- [ ] First blocks mined and propagated
- [ ] No chain forks detected
- [ ] Mempool functioning normally
- [ ] Network hashrate growing

### T+24 Hours
- [ ] Network stable with multiple miners
- [ ] Block time averaging correctly
- [ ] No consensus issues reported
- [ ] Community actively mining

## 🔒 **Immutable Consensus Rules**

**These values are FROZEN and locked before mainnet:**

```cpp
// src/daemon/consensus_subsidy.h
static constexpr uint64_t MAX_MINEABLE_SUPPLY = 99'000'000ULL * UNA_PER_DIN;
static constexpr uint32_t PHASE1_BLOCKS = 200'000;
static constexpr uint64_t PHASE1_REWARD = 100ULL * UNA_PER_DIN;
static constexpr uint64_t PHASE2_INITIAL_REWARD = 50ULL * UNA_PER_DIN;
static constexpr uint32_t HALVING_INTERVAL = 790'000;
static constexpr uint64_t GENESIS_BURN = 100'000ULL * UNA_PER_DIN;
```

**Changing these requires a hardfork and network-wide consensus.**

## 📊 **Success Metrics**

**Day 1:**
- [ ] 10+ active nodes
- [ ] 100+ blocks mined
- [ ] Multiple independent miners
- [ ] No chain forks

**Week 1:**
- [ ] 100+ active nodes
- [ ] 10,000+ blocks mined
- [ ] Community-run seed nodes
- [ ] Block explorers operational

**Month 1:**
- [ ] Distributed mining (no single entity >25%)
- [ ] Stable block time
- [ ] Active development community
- [ ] Exchange listings discussions

## 🚨 **Emergency Procedures**

### Critical Bug Found
1. Immediately notify all seed operators
2. Coordinate emergency patch
3. Test thoroughly before deployment
4. Coordinate upgrade window

### Chain Fork Detected
1. Identify fork point
2. Determine canonical chain (most work)
3. Notify miners/nodes
4. Investigate root cause

### Network Stall
1. Check seed nodes are online
2. Verify P2P connectivity
3. Check for difficulty adjustment issues
4. Coordinate miner participation

---

**Launch Coordinator**: _______________  
**Launch Date**: _______________  
**Genesis Hash**: `d6cd82dac8bcb2e2452f696d401be53ff9717ea1419d05e7fa88c98ad5cccdb7`  
**Status**: READY FOR MAINNET 🚀

