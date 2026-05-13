# Week 8: Production Hardening Roadmap

**Start Date**: November 8, 2025  
**Goal**: Operational confidence and network endurance  
**Status**: Structural work complete, transitioning to operations

---

## 🎯 **Mission: From Architecture to Production**

With Bitcoin Core-grade modularity achieved, focus shifts to:
- **Operational confidence**: CI/CD, testing, monitoring
- **Network endurance**: Multi-day testnet, peer stability
- **Security hardening**: Fuzzing, audit prep, threat modeling
- **Release readiness**: RC builds, signing, distribution

---

## 📋 **Phase 1: CI/CD Pipeline Finalization**

### **Goal**: Automated builds and tests on every PR

### **Tasks**

#### 1.1 Create GitHub Actions Workflow
**File**: `.github/workflows/build-and-test.yml`

```yaml
name: Build and Test

on:
  push:
    branches: [ main, feat/* ]
  pull_request:
    branches: [ main ]

jobs:
  build-macos-arm64:
    runs-on: macos-13-arm64
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive
      
      - name: Cache RocksDB
        uses: actions/cache@v3
        with:
          path: build/third_party/rocksdb
          key: ${{ runner.os }}-rocksdb-${{ hashFiles('third_party/rocksdb/**') }}
      
      - name: Cache Qt
        uses: actions/cache@v3
        with:
          path: ~/Qt
          key: ${{ runner.os }}-qt-6.9.1
      
      - name: Install Dependencies
        run: |
          brew install cmake openssl sqlite jsoncpp secp256k1
      
      - name: Configure CMake
        run: |
          cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
      
      - name: Build
        run: cmake --build build -j8
      
      - name: Run Tests
        run: |
          cd build
          ctest --output-on-failure
      
      - name: Upload Artifacts
        uses: actions/upload-artifact@v3
        with:
          name: macos-arm64-binaries
          path: |
            dinerod
            dinero-cli
            dinero-qt6.app
            build/lib*.a

  build-linux-x86_64:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive
      
      - name: Install Dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y \
            cmake g++ libssl-dev libsqlite3-dev \
            libjsoncpp-dev libsecp256k1-dev
      
      - name: Cache RocksDB
        uses: actions/cache@v3
        with:
          path: build/third_party/rocksdb
          key: ${{ runner.os }}-rocksdb-${{ hashFiles('third_party/rocksdb/**') }}
      
      - name: Configure CMake
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
      
      - name: Build
        run: cmake --build build -j$(nproc)
      
      - name: Run Tests
        run: |
          cd build
          ctest --output-on-failure
      
      - name: Upload Artifacts
        uses: actions/upload-artifact@v3
        with:
          name: linux-x86_64-binaries
          path: |
            dinerod
            dinero-cli
            build/lib*.a
```

**Time**: 2-3 hours  
**Priority**: HIGH

#### 1.2 Optimize Build Cache
- Cache RocksDB build (saves 5-7 min)
- Cache Qt build (saves 10+ min on macOS)
- Target: < 10 min total CI time

#### 1.3 Test Suite Integration
- Run all 30 regression tests in CI
- Fail on any test failure
- Generate test report artifacts

---

## 📋 **Phase 2: Long-Running Testnet (7 Days)**

### **Goal**: Zero memory leaks and no forks for ≥ 168 hours

### **Infrastructure Setup**

#### 2.1 Deploy 3-5 Nodes
```
Node 1 (us-east-1):    Virginia (seed)
Node 2 (us-west-2):    California
Node 3 (eu-central-1): Frankfurt
Node 4 (ap-southeast-1): Singapore (optional)
Node 5 (local):        Development machine
```

#### 2.2 Configuration
**File**: `testnet-7day.conf`

```ini
# 7-Day Testnet Configuration
testnet=1
server=1
daemon=1

# RPC
rpcport=20996
rpcallowip=0.0.0.0/0
rpcuser=testnet_user
rpcpassword=<generate_strong_password>

# P2P
port=21001
maxconnections=125

# Logging
debug=net
debug=rpc
debug=mempool
logips=1

# Metrics
metrics_port=9090
enable_prometheus=1

# Explorer sync
explorer_sync_enabled=1
explorer_sync_interval=10
```

#### 2.3 Monitoring Stack
```bash
# Prometheus config
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: 'dinero-nodes'
    static_configs:
      - targets:
        - 'node1:9090'
        - 'node2:9090'
        - 'node3:9090'

# Grafana dashboards (already built)
- Chain height per node
- Peer count over time
- Explorer sync lag
- RPC latency (p50, p95, p99)
- Memory usage
- CPU usage
```

### **Metrics to Collect**

| Metric | Target | Critical If |
|--------|--------|-------------|
| Peer uptime | > 99% | < 95% |
| Chain sync lag | < 5 blocks | > 50 blocks |
| Explorer sync lag | < 10 blocks | > 100 blocks |
| RPC latency (p95) | < 100ms | > 500ms |
| Memory growth | < 1MB/day | > 10MB/day |
| Fork events | 0 | > 0 |

**Time**: 7 days continuous + 1 day setup  
**Priority**: HIGH

---

## 📋 **Phase 3: Fuzz & Regression Integration**

### **Goal**: Catch edge cases and memory errors automatically

### **Tasks**

#### 3.1 Enable Fuzzing in CI
**File**: `.github/workflows/nightly-fuzz.yml`

```yaml
name: Nightly Fuzzing

on:
  schedule:
    - cron: '0 2 * * *'  # 2 AM daily
  workflow_dispatch:

jobs:
  fuzz:
    runs-on: ubuntu-22.04
    strategy:
      matrix:
        target:
          - fuzz_transaction_deserializer
          - fuzz_block_parser
          - fuzz_address_decoder
          - fuzz_rpc_handler
          - fuzz_p2p_message
    
    steps:
      - uses: actions/checkout@v3
      
      - name: Build with Fuzzing
        run: |
          cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Debug \
            -DENABLE_FUZZING=ON \
            -DCMAKE_CXX_FLAGS="-fsanitize=address,fuzzer"
          cmake --build build --target ${{ matrix.target }}
      
      - name: Run Fuzzer (30 min)
        run: |
          mkdir -p corpus/${{ matrix.target }}
          timeout 1800 ./build/${{ matrix.target }} \
            corpus/${{ matrix.target }} \
            -max_total_time=1800 \
            -print_final_stats=1 \
            -artifact_prefix=crashes/
        continue-on-error: true
      
      - name: Upload Crashes
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: fuzz-crashes-${{ matrix.target }}
          path: crashes/
```

#### 3.2 Corpus Management
```bash
# Store seed corpus
tests/fuzz/corpus/
├── transaction/       # Valid transactions
├── block/            # Valid blocks
├── address/          # Valid addresses
├── rpc/              # Valid RPC requests
└── p2p/              # Valid P2P messages

# On crash, analyze:
./build/fuzz_target crashes/crash-xyz
# Fix bug, add to regression tests
```

**Time**: 2-3 hours setup + ongoing  
**Priority**: MEDIUM

---

## 📋 **Phase 4: Security Audit Prep**

### **Goal**: Be audit-ready with comprehensive threat model

### **Tasks**

#### 4.1 RPC Input Sanitization Audit
```cpp
// Check all RPC handlers for:
✅ Numeric bounds (height, amount, count)
✅ JSON schema validation
✅ String length limits
✅ Address format validation
✅ SQL injection prevention (if any raw SQL)
✅ Command injection prevention
```

**File**: `docs/RPC_SECURITY_AUDIT.md`

#### 4.2 Wallet Key Management Review
```
Checklist:
✅ HD seed generation (uses /dev/urandom or SecRandomCopyBytes)
✅ Seed encryption (PBKDF2-HMAC-SHA512 with user passphrase)
✅ Key derivation (BIP32/BIP44 correct implementation)
✅ Private keys never logged
✅ Seed never leaves encrypted storage
✅ Backup mechanism secure (encrypted)
```

**File**: `docs/WALLET_SECURITY_REVIEW.md`

#### 4.3 Peer Score Threshold Tests
```cpp
// Ensure banlist cannot overflow
TEST(P2PTest, BanlistCapacity) {
    P2PManager p2p;
    for (int i = 0; i < 10000; i++) {
        p2p.banPeer("192.168.0." + std::to_string(i % 255));
    }
    ASSERT_LT(p2p.getBanlistSize(), 1000);  // Should have cap
}

TEST(P2PTest, PeerScoreUnderflow) {
    Peer peer;
    for (int i = 0; i < 1000; i++) {
        peer.decreaseScore(100);
    }
    ASSERT_GE(peer.getScore(), 0);  // Should not underflow
}
```

#### 4.4 Threat Model Document
**File**: `docs/SECURITY_THREAT_MODEL.md`

```markdown
# Dinero Core Security Threat Model

## Assets
1. User private keys (HD seed)
2. Blockchain state (consensus)
3. UTXO set integrity
4. Network availability

## Threats

### T1: Private Key Theft
- **Attack**: Malware reads wallet.db
- **Mitigation**: Encryption with user passphrase (PBKDF2)
- **Residual Risk**: Keylogger can capture passphrase

### T2: Consensus Manipulation
- **Attack**: Eclipse attack + invalid block injection
- **Mitigation**: PoW validation, peer diversity, checkpoints
- **Residual Risk**: 51% attack (hashrate dependent)

### T3: DoS via RPC
- **Attack**: Flood RPC with expensive queries
- **Mitigation**: Rate limiting, authentication, query limits
- **Residual Risk**: Authenticated user can still DoS

### T4: UTXO Set Corruption
- **Attack**: Disk corruption or bug in ChainDB
- **Mitigation**: RocksDB checksums, reindex capability
- **Residual Risk**: Undetected corruption could fork chain

### T5: P2P Network Partition
- **Attack**: Sybil attack to isolate node
- **Mitigation**: Peer scoring, connection diversity, seed nodes
- **Residual Risk**: Determined attacker with many IPs

## Security Controls
- [x] Wallet encryption (PBKDF2-HMAC-SHA512)
- [x] RPC authentication (cookie-based)
- [x] PoW validation (SHA-256d)
- [x] Peer scoring system
- [x] Rate limiting (planned)
- [ ] 2FA for wallet unlock (future)
- [ ] Hardware wallet support (in progress)
```

**Time**: 4-6 hours  
**Priority**: HIGH

---

## 📋 **Phase 5: Release Candidate Process**

### **Goal**: Signed, reproducible binaries for all platforms

### **Tasks**

#### 5.1 Version Tagging
```bash
# Tag release candidate
git tag -a v1.1-rc1 -m "Release Candidate 1
- Bitcoin Core-grade modularity
- Three persistence layers isolated
- ExplorerDB analytics layer
- 296+ blocks on mainnet

Changes since v1.0:
- feat: ExplorerDB service (Phase 1-5)
- refactor: CMake isolation
- fix: Regtest genesis block
- docs: 1,360+ lines of architecture docs
"

git push origin v1.1-rc1
```

#### 5.2 Build Matrix
```
Platforms:
- macOS ARM64 (Apple Silicon)
- macOS x86_64 (Intel)
- Linux x86_64
- Linux ARM64 (Raspberry Pi)
- Windows x86_64 (optional)

Artifacts:
- dinerod (daemon)
- dinero-cli (CLI tool)
- dinero-qt6 (GUI wallet) [macOS only initially]
- libdinero_wallet.a (for integrations)
```

#### 5.3 Smoke Tests
```bash
# CLI wallet connectivity
./dinero-cli -rpcport=20998 getblockcount
./dinero-cli -rpcport=20998 getnewaddress
./dinero-cli -rpcport=20998 getbalance

# GUI wallet
# 1. Launch dinero-qt6
# 2. Generate address
# 3. Send transaction
# 4. Verify balance updates

# P2P connectivity
./dinerod -testnet &
sleep 10
./dinero-cli -testnet getpeerinfo | jq length
# Should have > 0 peers
```

#### 5.4 Signed Hashes & Manifest
**File**: `dinero-v1.1-rc1-manifest.json`

```json
{
  "version": "1.1-rc1",
  "release_date": "2025-11-15",
  "git_tag": "v1.1-rc1",
  "git_commit": "c3bbb41f2",
  "binaries": {
    "macos-arm64": {
      "file": "dinero-1.1-rc1-macos-arm64.tar.gz",
      "sha256": "<compute>",
      "size_bytes": 55123456
    },
    "linux-x86_64": {
      "file": "dinero-1.1-rc1-linux-x86_64.tar.gz",
      "sha256": "<compute>",
      "size_bytes": 52345678
    }
  },
  "signature": {
    "gpg_key": "0x1234567890ABCDEF",
    "signature_file": "dinero-v1.1-rc1-manifest.json.asc"
  }
}
```

```bash
# Sign manifest
gpg --detach-sign --armor dinero-v1.1-rc1-manifest.json

# Verify
gpg --verify dinero-v1.1-rc1-manifest.json.asc
```

**Time**: 1 day  
**Priority**: MEDIUM

---

## 📊 **Week 8 Timeline**

| Day | Phase | Tasks | Priority |
|-----|-------|-------|----------|
| **Day 1** | CI/CD | GitHub Actions workflow | HIGH |
| **Day 2** | CI/CD | Test integration, caching | HIGH |
| **Day 3** | Testnet | Deploy 3-5 nodes | HIGH |
| **Day 4** | Testnet | Monitoring setup (Prometheus/Grafana) | HIGH |
| **Day 5-11** | Testnet | 7-day endurance test | HIGH |
| **Day 6** | Security | RPC audit, wallet review | HIGH |
| **Day 7** | Security | Threat model document | HIGH |
| **Day 8** | Fuzzing | Nightly fuzz workflow | MEDIUM |
| **Day 12** | Testnet | Analyze 7-day results | HIGH |
| **Day 13** | RC | Tag v1.1-rc1, build artifacts | MEDIUM |
| **Day 14** | RC | Smoke tests, sign hashes | MEDIUM |

---

## ✅ **Success Criteria**

| Metric | Target | Status |
|--------|--------|--------|
| CI build time | < 10 min | 🔄 Pending |
| Test pass rate | 100% (30/30) | 🔄 Pending |
| Testnet uptime | > 99% over 7 days | 🔄 Pending |
| Memory leaks | 0 detected | 🔄 Pending |
| Fork events | 0 in 7 days | 🔄 Pending |
| RPC latency (p95) | < 100ms | 🔄 Pending |
| Security issues | 0 critical | 🔄 Pending |
| RC builds | All platforms | 🔄 Pending |

---

## 🎯 **Immediate Next Steps** (Priority Order)

1. **✅ Phase 1.1**: Create GitHub Actions workflow (2-3 hours)
2. **✅ Phase 2.1**: Deploy testnet nodes (2 hours)
3. **✅ Phase 2.2**: Configure monitoring (1 hour)
4. **✅ Phase 4**: Start security audit prep (4-6 hours)
5. **🔄 Phase 2.3**: Let testnet run for 7 days (background)
6. **Phase 3**: Enable nightly fuzzing (2 hours)
7. **Phase 5**: Prepare RC build process (1 day)

---

## 📝 **Documentation Deliverables**

1. ✅ `WEEK8_PRODUCTION_HARDENING.md` (this file)
2. 🔄 `RPC_SECURITY_AUDIT.md` (Phase 4.1)
3. 🔄 `WALLET_SECURITY_REVIEW.md` (Phase 4.2)
4. 🔄 `SECURITY_THREAT_MODEL.md` (Phase 4.4)
5. 🔄 `TESTNET_7DAY_REPORT.md` (after Phase 2)

---

## 🎊 **Conclusion**

With Bitcoin Core-grade modularity achieved, Week 8 focuses on proving
Dinero Core can handle production workloads with confidence:

- **CI/CD**: Catch regressions early
- **Testnet**: Prove network endurance  
- **Fuzzing**: Find edge cases automatically
- **Security**: Be audit-ready
- **RC**: Deliver signed, reproducible builds

**Status after Week 8**: Production-ready with operational confidence ✅

---

**Created**: November 7, 2025  
**Author**: Dinero Core Team  
**Status**: Ready to execute 🚀


