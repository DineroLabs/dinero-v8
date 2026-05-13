# Week 7 Roadmap: CI/CD & Observability Rollout

**Goal**: Formalize reliability through automation and monitoring
**Status**: Planning Phase
**Prerequisites**: ✅ Week 6 Day 1 Complete (Production-Ready Core)

---

## 🎯 Week Objectives

1. **Continuous Integration** - Automatic test verification on every build
2. **Observability** - Real-time metrics and monitoring dashboards
3. **Release Engineering** - First public testnet binary (v1.0.0-core-stable)
4. **Integration Tests** - Expand test coverage to RPC and P2P layers
5. **Documentation** - CI/CD setup guides and deployment procedures

---

## 📅 Day-by-Day Plan

### **Day 1: CI/CD Foundation**

#### Tasks
1. **GitHub Actions Setup**
   ```yaml
   # .github/workflows/test.yml
   name: Test Suite
   on: [push, pull_request]
   jobs:
     test:
       runs-on: ubuntu-latest
       steps:
         - uses: actions/checkout@v3
         - name: Build
           run: cmake --build build
         - name: Test
           run: ctest --output-on-failure --test-dir build
   ```

2. **Build Verification**
   - Add `make check` target
   - Enforce 100% test pass before merge
   - Add build status badge to README

3. **Pre-commit Hooks**
   ```bash
   # .git/hooks/pre-commit
   #!/bin/bash
   cmake --build build --target test_mining_smoke
   ./build/test_mining_smoke
   ```

#### Deliverables
- ✅ GitHub Actions workflow operational
- ✅ Build badge in README
- ✅ Pre-commit hooks installed

#### Success Criteria
- Every PR automatically runs tests
- Failed tests block merge
- Build time < 3 minutes

---

### **Day 2: Metrics & Monitoring**

#### Tasks
1. **Prometheus Exporter**
   ```cpp
   // Extend /metrics endpoint
   GET /metrics

   # TYPE dinero_blocks_mined counter
   dinero_blocks_mined{miner_id="miner1"} 42

   # TYPE dinero_difficulty gauge
   dinero_difficulty 0x1f00ffff

   # TYPE dinero_peers gauge
   dinero_peers 8
   ```

2. **Grafana Dashboard**
   - Import Dinero Core dashboard template
   - Panels:
     - Block height over time
     - Difficulty adjustment
     - Hashrate estimate
     - Peer count
     - Mining blocks found (per miner)

3. **Alerting Rules**
   ```yaml
   # prometheus/alerts.yml
   - alert: NoBlocksIn10Minutes
     expr: rate(dinero_blocks_mined[10m]) == 0
     annotations:
       summary: "No blocks mined in 10 minutes"
   ```

#### Deliverables
- ✅ `/metrics` endpoint with full data
- ✅ Grafana dashboard template
- ✅ Basic alerting rules

#### Success Criteria
- Metrics update in real-time
- Dashboard visualizes network health
- Alerts fire correctly

---

### **Day 3: Integration Test Expansion**

#### Tasks
1. **P2P Integration Test**
   ```cpp
   TEST(P2PIntegration, BlockPropagation) {
       // Create two test nodes
       auto node1 = TestDaemonContext();
       auto node2 = TestDaemonContext();

       // Connect them
       node1.p2p->ConnectTo("127.0.0.1:20001");

       // Mine block on node1
       auto job = node1.mining->CreateJob();
       // ... solve and submit

       // Verify node2 receives block
       EXPECT_EQ(node2.chainstate->GetHeight(), 1);
   }
   ```

2. **Wallet Integration Test**
   ```cpp
   TEST(WalletIntegration, MiningRewardCredited) {
       auto ctx = TestDaemonContext();

       // Mine block to wallet address
       auto job = ctx.mining->CreateJob();
       // ... solve and submit

       // Verify wallet balance updated
       uint64_t balance = ctx.wallet->GetBalance();
       EXPECT_EQ(balance, 100 * COIN);  // 100 DIN
   }
   ```

3. **Reorg Simulation Test**
   ```cpp
   TEST(ConsensusIntegration, ChainReorganization) {
       auto ctx = TestDaemonContext();

       // Create competing chain branches
       // ... mine blocks on both

       // Submit longer chain
       // Verify reorg occurs
       EXPECT_EQ(ctx.chainstate->GetBestChain(), longer_chain);
   }
   ```

4. **RPC End-to-End Test**
   ```cpp
   TEST(RPCIntegration, FullMiningFlow) {
       auto ctx = TestDaemonContext();

       // Call getblocktemplate
       auto response = ctx.rpc->Call("getblocktemplate");

       // Solve block (regtest difficulty)
       auto solved_block = SolveBlock(response);

       // Call submitblock
       auto result = ctx.rpc->Call("submitblock", solved_block);
       EXPECT_EQ(result, "success");

       // Verify chain advanced
       EXPECT_EQ(ctx.chainstate->GetHeight(), 1);
   }
   ```

#### Deliverables
- ✅ 4 new integration tests
- ✅ Test coverage > 80%
- ✅ All tests passing

#### Success Criteria
- Integration tests run in CI
- Tests catch real bugs
- Execution time < 1 second each

---

### **Day 4: Release Engineering**

#### Tasks
1. **Version Tagging**
   ```bash
   # Create release tag
   git tag -a v1.0.0-core-stable \
       -m "First production-ready Dinero Core

   Features:
   - 100% context-driven architecture
   - Verified consensus (262.8M DIN hard cap)
   - 100% passing test suite (5/5 tests)
   - Production-ready core infrastructure

   Changelog:
   - Fixed 4 critical consensus bugs
   - Complete P2P migration
   - Metrics system implemented
   - Comprehensive documentation"

   git push origin v1.0.0-core-stable
   ```

2. **Binary Builds**
   ```bash
   # Build for all platforms
   # Linux x64
   cmake -B build-linux-x64 -DCMAKE_BUILD_TYPE=Release
   cmake --build build-linux-x64
   tar -czf dinero-v1.0.0-linux-x64.tar.gz build-linux-x64/dinero*

   # macOS ARM64
   cmake -B build-macos-arm64 -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_OSX_ARCHITECTURES=arm64
   cmake --build build-macos-arm64
   tar -czf dinero-v1.0.0-macos-arm64.tar.gz build-macos-arm64/dinero*

   # Generate checksums
   shasum -a 256 dinero-v1.0.0-*.tar.gz > SHA256SUMS.txt
   ```

3. **Release Notes**
   - Create `RELEASE_NOTES_v1.0.0.md`
   - Include upgrade instructions
   - List breaking changes (none for v1.0.0)
   - Document new features

4. **Testnet Deployment Guide**
   ```markdown
   # Testnet Deployment Guide

   ## Quick Start
   ```bash
   # Extract binary
   tar -xzf dinero-v1.0.0-linux-x64.tar.gz

   # Run testnet node
   ./dinerod --testnet --datadir=~/.dinero-testnet
   ```

   ## Mining on Testnet
   ```bash
   # Generate wallet address
   ./dinero-cli --testnet wallet.create

   # Start mining
   ./dinero-cli --testnet mining.start <address>
   ```
   ```

#### Deliverables
- ✅ Release tag created
- ✅ Binaries for Linux, macOS, Windows
- ✅ SHA256 checksums
- ✅ Release notes
- ✅ Testnet deployment guide

#### Success Criteria
- Binaries run on target platforms
- Checksums verify correctly
- Documentation is clear

---

### **Day 5: Documentation & Polish**

#### Tasks
1. **CI/CD Documentation**
   ```markdown
   # docs/CI_CD_SETUP.md

   ## GitHub Actions
   - Test workflow runs on every push
   - Build artifacts saved for 30 days
   - Test results visible in PR

   ## Local Testing
   ```bash
   # Run tests before commit
   cmake --build build
   ctest --output-on-failure --test-dir build
   ```
   ```

2. **Metrics Documentation**
   ```markdown
   # docs/METRICS.md

   ## Available Metrics

   | Metric | Type | Description |
   |--------|------|-------------|
   | `dinero_blocks_mined` | counter | Blocks found by miner |
   | `dinero_difficulty` | gauge | Current difficulty |
   | `dinero_height` | gauge | Chain tip height |
   | `dinero_peers` | gauge | Connected peers |

   ## Grafana Setup
   1. Install Grafana
   2. Add Prometheus data source
   3. Import dashboard: `grafana/dinero_dashboard.json`
   ```

3. **Architecture Diagram Update**
   - Add CI/CD pipeline diagram
   - Add metrics flow diagram
   - Update service topology

4. **Code Cleanup**
   - Remove `.bak` files
   - Remove commented-out code
   - Fix linter warnings

#### Deliverables
- ✅ CI/CD setup guide
- ✅ Metrics documentation
- ✅ Architecture diagrams updated
- ✅ Code cleanup complete

#### Success Criteria
- New contributors can set up CI easily
- Metrics are well-documented
- Codebase is clean

---

## 📊 Week 7 Success Metrics

| Metric | Target | Status |
|--------|--------|--------|
| CI/CD operational | ✅ Yes | Pending |
| Test suite automated | ✅ Yes | Pending |
| Metrics dashboard live | ✅ Yes | Pending |
| Integration tests added | 4+ tests | Pending |
| Release tagged | v1.0.0 | Pending |
| Binaries built | 3 platforms | Pending |
| Documentation complete | 100% | Pending |

---

## 🚀 Week 8 Preview: Hybrid Consensus Prototype

### Goal
Experiment with PoW+PoS hybrid consensus using the `IConsensusEngine` interface.

### Approach
1. Implement `PosConsensusEngine` (stake weight validation)
2. Implement `HybridConsensusEngine` (PoW + PoS coordinator)
3. Run parallel miners in regtest
4. Benchmark performance

### Why This is Easy Now
- Service isolation complete ✅
- Context injection in place ✅
- Test infrastructure ready ✅
- No need to touch Chainstate/Mempool/Mining ✅

---

## 💡 Strategic Benefits

### Immediate (Week 7)
- **Reliability**: CI prevents regression bugs
- **Visibility**: Metrics enable monitoring
- **Confidence**: Public testnet validates stability

### Medium-term (Weeks 8-10)
- **Innovation**: Can safely experiment with consensus
- **Community**: Public binary enables testing
- **Feedback**: Real-world usage data

### Long-term (Months 3-6)
- **Mainnet Launch**: Production-ready infrastructure
- **Adoption**: Professional-grade project attracts users
- **Sustainability**: Maintainable codebase scales

---

## 🎯 Definition of Done (Week 7)

Week 7 is complete when:

1. ✅ CI/CD pipeline automatically runs tests on every PR
2. ✅ Grafana dashboard displays real-time network metrics
3. ✅ Integration test suite has 4+ new tests (all passing)
4. ✅ Release v1.0.0-core-stable is tagged and binaries built
5. ✅ Testnet deployment guide published
6. ✅ All documentation updated and comprehensive

**Status after Week 7**: 🟢 **Ready for Public Testnet Launch**

---

## 📚 Resources

### Tools Required
- GitHub Actions (free for public repos)
- Prometheus (open source)
- Grafana (open source)
- Docker (for consistent builds)

### Documentation References
- [GitHub Actions Docs](https://docs.github.com/en/actions)
- [Prometheus Docs](https://prometheus.io/docs/)
- [Grafana Docs](https://grafana.com/docs/)
- [GoogleTest Docs](https://google.github.io/googletest/)

---

## 🏁 Week 7 Kickoff

**Start Date**: TBD
**Prerequisites**: ✅ Week 6 Day 1 Complete

**First Action**: Set up GitHub Actions workflow for automatic testing.

---

*"Automation is the foundation of reliability. Monitoring is the foundation of confidence."*
— Week 7 Principle
