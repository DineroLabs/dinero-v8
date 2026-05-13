# DineroCoin v3.0.0-alpha1 Release Plan

**Date:** 2026-01-11
**Status:** Ready for Implementation
**Target:** v3.0.0-alpha1 Pre-Release

---

## Executive Summary

This document provides a **complete roadmap** from current state (main branch) to v3.0.0-alpha1 release. All supporting documentation has been created and is referenced below.

**Current State:**
- ✅ Protocol complete (Stateless, Lightning, Mobile)
- ✅ Tests comprehensive (421 test files)
- ✅ Build system mature (CMake, multi-platform)
- ✅ 842 commits since December (active development)

**Deliverables Created:**
1. **Release Readiness Audit** (`docs/RELEASE_READINESS_AUDIT.md`)
2. **CI Pipeline Design** (`docs/CI_PIPELINE_V3_ALPHA.md`)
3. **Smoke Tests Specification** (`docs/SMOKE_TESTS_SPECIFICATION.md`)
4. **Alpha Cut Criteria** (`docs/V3_ALPHA1_CUT_CRITERIA.md`)

**Recommendation:** ✅ **Proceed with v3.0.0-alpha1**

---

## Release Philosophy

### What Alpha Means

**Alpha releases are:**
- ✅ Functionally complete (protocol done)
- ✅ Testable by developers
- ✅ Suitable for testnet deployment
- ⚠️ Not production-ready (no audit)
- ⚠️ May have incomplete features (Lightning wallet)
- ⚠️ May have performance issues

**Alpha releases are NOT:**
- ❌ For mainnet use
- ❌ For exchanges
- ❌ Security audited
- ❌ Performance optimized
- ❌ Feature-complete (all nice-to-haves)

### Version Semantics

```
v3.0.0-alpha1
│ │ │  └─────── Pre-release identifier (alpha, beta, rc)
│ │ └────────── Patch version (0 for new major)
│ └──────────── Minor version (0 for new major)
└────────────── Major version (3 = breaking protocol change)
```

**Rationale for v3.0.0:**
- Stateless validation is a generational change
- Not a patch (v2.3) or minor update (v2.10)
- Deserves major version bump

---

## Phase-Based Release Strategy

### Phase A: Pre-Release Preparation (Option A - Current) ✅

**Status:** You are here

**Characteristics:**
- Last public release: v2.2.6
- Main branch: Far ahead with v3.0 protocol
- No new release yet (packaging gap only)

**What This Means:**
- Your GitHub Releases page shows v2.2.6 (stale)
- Your code is v3.0 (advanced)
- This is the correct state until binaries exist

**Verdict:** ✅ **Disciplined approach**

### Phase B: Pre-Release (Option B - Next Step)

**Target:** Cut v3.0.0-alpha1

**Actions:**
1. Tag `v3.0.0-alpha1` on main
2. Build binaries (macOS, Linux, Windows)
3. Create GitHub pre-release (marked as alpha)
4. Attach binaries + checksums
5. Add security warning

**Outcome:**
- Signals major protocol jump
- Allows developer testing
- No production recommendation
- Honest about alpha status

**Timeline:** 2-3 days after preparation complete

### Phase C: Full Release (Option C - Future)

**Target:** v3.0.0 (final)

**Prerequisites:**
- v3.0.0-alpha1 tested (2+ weeks)
- v3.0.0-beta1 tested (4+ weeks)
- External security audit complete
- All P0/P1 bugs fixed
- Testnet deployment successful

**Timeline:** Weeks to months after alpha

---

## Audit Findings Summary

**Full Report:** `docs/RELEASE_READINESS_AUDIT.md`

### ✅ What's Ready

1. **Protocol:** Complete (Phases 8-12 done)
2. **Tests:** 421 test files, comprehensive coverage
3. **Build System:** CMake, multi-platform, release-grade
4. **Git Health:** 842 commits since Dec, clean history
5. **Documentation:** Protocol documented, user docs adequate

### ⚠️ What Needs Attention

1. **Tests disabled in builds** → Enable with `-DENABLE_TESTS=ON`
2. **881 TODOs in codebase** → Review consensus TODOs, tag others
3. **No security audit** → Label as alpha, warn users
4. **Lightning incomplete** → Signature verification, payment routing (P2)
5. **Documentation updates** → README, CHANGELOG, SECURITY.md

### ❌ Blockers (None Found)

- No data corruption bugs
- No consensus split risks
- No build failures (verified locally)
- No critical security issues identified

**Verdict:** ✅ **Ready for alpha** with minor preparation

---

## CI Pipeline Summary

**Full Design:** `docs/CI_PIPELINE_V3_ALPHA.md`

### Build Matrix

| Platform | Runner | Arch | Static Linking | Tests |
|----------|--------|------|----------------|-------|
| macOS | macos-14 | arm64 | ✅ | ✅ |
| macOS | macos-13 | x86_64 | ✅ | ✅ |
| Linux | ubuntu-22.04 | x86_64 | ✅ | ✅ |
| Windows | windows-2022 | x86_64 | ⚠️ | ✅ |

### Pipeline Stages

1. **Environment Setup** - Install build dependencies
2. **Checkout** - Clone repo with submodules
3. **Configure** - CMake with DINERO_RELEASE=ON
4. **Build** - Compile dinerod, dinero-cli, tests
5. **Test** - Run Ring, consensus, mobile, Lightning tests
6. **Smoke Test** - Verify daemon starts, RPC works
7. **Verify Binary** - Check for unwanted dependencies
8. **Package** - Create tar.gz/zip with checksums
9. **Upload** - GitHub Actions artifacts
10. **Release** - Create GitHub pre-release

### Artifacts Produced

```
DineroCoin-v3.0.0-alpha1-macos-arm64.tar.gz      (+ .sha256)
DineroCoin-v3.0.0-alpha1-macos-x86_64.tar.gz     (+ .sha256)
DineroCoin-v3.0.0-alpha1-linux-x86_64.tar.gz     (+ .sha256)
DineroCoin-v3.0.0-alpha1-windows-x86_64.zip      (+ .sha256)
SHA256SUMS
SHA256SUMS.asc (GPG signature)
BUILD_ATTESTATION.json
```

---

## Smoke Tests Summary

**Full Specification:** `docs/SMOKE_TESTS_SPECIFICATION.md`

### Test Coverage

**16 core tests per platform:**
1. Binary exists and is executable
2. Daemon starts in regtest mode
3. Daemon process is running
4. RPC responds to ping
5. Get blockchain info works
6. Generate new address
7. Initial balance is 0
8. Generate 101 blocks
9. Balance > 0 after mining
10. List unspent outputs
11. Block count = 101
12. Get best block hash
13. Stop daemon cleanly
14. Daemon process terminated
15. Platform-specific checks (no Homebrew/gRPC deps)
16. Binary format verification

### Platform-Specific

**macOS:**
- Verify binary architecture (arm64/x86_64)
- Verify no Homebrew dependencies (`otool -L`)
- Verify GUI bundle structure (if built)

**Linux:**
- Verify ELF binary format
- Verify no gRPC/protobuf/abseil (`ldd`)

**Windows:**
- Verify PE executable format
- Verify DLLs bundled

**Execution:** < 2 minutes per platform

---

## Alpha Cut Criteria Summary

**Full Criteria:** `docs/V3_ALPHA1_CUT_CRITERIA.md`

### Go/No-Go Checklist

#### Required (Must Be Green) ✅
- [ ] Protocol complete (Phase 8-12)
- [ ] macOS build passes
- [ ] Linux build passes
- [ ] Windows build passes
- [ ] Ring tests pass (65/65)
- [ ] Stateless validation tests pass
- [ ] Smoke tests pass (all platforms)
- [ ] README.md updated
- [ ] CHANGELOG.md entry created
- [ ] No P0 bugs

#### Recommended (Should Be Green) ⚠️
- [ ] Mobile tests pass (T12.1-T12.9)
- [ ] Lightning tests pass
- [ ] Binary verification passes
- [ ] Security notice added
- [ ] No P1 bugs (or documented)

#### Optional (Nice to Have) ✅
- [ ] Wallet tests pass
- [ ] Reproducible builds verified
- [ ] Performance benchmarks run
- [ ] GUI builds (dinero-qt)

### Decision Matrix

| Required | Recommended | Decision |
|----------|-------------|----------|
| ✅ All | ✅ All | ✅ **SHIP IT** |
| ✅ All | ⚠️ Some | ✅ **SHIP IT** |
| ⚠️ Some | - | ❌ **HOLD** |

---

## Implementation Roadmap

### Day 1: Preparation

**Morning:**
1. Create smoke test script
   ```bash
   # Create scripts/smoke-test.sh from spec
   chmod +x scripts/smoke-test.sh
   ```

2. Update documentation
   ```bash
   # Update README.md (add v3.0 features, alpha warning)
   # Create CHANGELOG.md entry for v3.0.0-alpha1
   # Create/update SECURITY.md
   ```

3. Review known issues
   ```bash
   # Check GitHub issues for P0/P1 bugs
   # Review consensus TODOs
   # Document known limitations
   ```

**Afternoon:**
4. Create CI workflow
   ```bash
   # Create .github/workflows/v3-alpha-release.yml
   # Copy from CI_PIPELINE_V3_ALPHA.md
   ```

5. Enable tests in CI
   ```yaml
   # Add -DENABLE_TESTS=ON to CMake flags
   ```

6. Test workflow locally
   ```bash
   # Run smoke test script
   # Run build with tests enabled
   ```

### Day 2: Verification

**Morning:**
7. Push CI workflow to feature branch
   ```bash
   git checkout -b release/v3.0.0-alpha1-prep
   git add .github/workflows/v3-alpha-release.yml
   git add scripts/smoke-test.sh
   git add README.md CHANGELOG.md SECURITY.md
   git commit -m "feat: add v3.0.0-alpha1 release infrastructure"
   git push origin release/v3.0.0-alpha1-prep
   ```

8. Test CI workflow with `workflow_dispatch`
   ```
   # Go to GitHub Actions
   # Run workflow manually
   # Verify all platforms build
   ```

**Afternoon:**
9. Fix platform-specific issues (if any)
   ```bash
   # Address build failures
   # Fix test failures
   # Update smoke test script if needed
   ```

10. Merge to main when green
    ```bash
    # Create PR from feature branch
    # Get maintainer review
    # Merge when CI passes
    ```

### Day 3: Release

**Morning:**
11. Final review
    ```bash
    # Review Go/No-Go checklist
    # Ensure all Required criteria met
    # Get sign-off from maintainer
    ```

12. Tag release
    ```bash
    git checkout main
    git pull origin main
    git tag -a v3.0.0-alpha1 -m "DineroCoin v3.0.0-alpha1 - Alpha Release

    Protocol v3.0 features:
    - Stateless validation (Utreexo)
    - Proof network (cache, routing, gossip)
    - Lightning integration (read-only client)
    - Mobile profile (compiler-enforced)

    ⚠️ Alpha release - testnet use only, no security audit"

    git push origin v3.0.0-alpha1
    ```

**Afternoon:**
13. Monitor CI build
    ```
    # Watch GitHub Actions
    # Verify artifacts created
    # Download and test binaries
    ```

14. Create GitHub release
    ```
    # Go to Releases → Draft a new release
    # Select tag: v3.0.0-alpha1
    # Mark as "Pre-release"
    # Add release notes (see template below)
    # Attach CI artifacts (if not automatic)
    # Publish
    ```

15. Announce
    ```
    # Post to GitHub Discussions
    # Tweet/social media
    # Notify testers
    ```

---

## Release Notes Template

**Copy this to GitHub release:**

```markdown
# DineroCoin v3.0.0-alpha1 🚀

## ⚠️ ALPHA RELEASE - NOT FOR PRODUCTION

This is the first alpha release of DineroCoin v3.0, featuring a complete protocol redesign for stateless validation, Lightning integration, and mobile support.

**⚠️ Important Warnings:**
- ❌ **NOT for mainnet use**
- ❌ **NOT externally audited**
- ✅ **Testnet use only**
- ✅ **Feedback welcome!**

---

## What's New in v3.0

### 🎯 Stateless Validation (Utreexo)
Full nodes can validate the blockchain without storing the UTXO database, using cryptographic proofs.

**Benefits:**
- Drastically reduced storage requirements
- Mobile-friendly node operation
- Same security as full nodes

### 🌐 Proof Distribution Network
Efficient proof distribution infrastructure:
- LRU + TTL proof cache
- Multi-peer proof routing
- P2P proof gossip (INV_PROOF)
- ZSTD compression (~40-60% size reduction)

### ⚡ Lightning Integration
Read-only Lightning client with proof-based validation:
- Stateless watchtower (no UTXO DB required)
- Channel funding verification
- HTLC output verification
- Mobile-friendly Lightning operations

### 📱 Mobile Profile
Compiler-enforced resource limits for iOS/Android:
- 16 MB proof cache
- 30-second burst mode (iOS background)
- Battery-friendly (estimated 1-3% daily)
- App Store compliance enforced at compile time

### 🔄 Sync Validation
Real-world sync scenarios tested and validated:
- Resumable sync after interruption
- Cache eviction handling
- Network partition recovery
- Monotonic validation progress

---

## Downloads

| Platform | Architecture | Download | SHA256 |
|----------|--------------|----------|--------|
| **macOS** | arm64 (M1/M2/M3) | [Download](TBD) | [Checksum](TBD) |
| **macOS** | x86_64 (Intel) | [Download](TBD) | [Checksum](TBD) |
| **Linux** | x86_64 | [Download](TBD) | [Checksum](TBD) |
| **Windows** | x86_64 | [Download](TBD) | [Checksum](TBD) |

### Verification

```bash
# macOS/Linux
shasum -a 256 -c DineroCoin-v3.0.0-alpha1-*.tar.gz.sha256

# Windows (PowerShell)
(Get-FileHash DineroCoin-*.zip).Hash -eq (Get-Content DineroCoin-*.zip.sha256).Split()[0]
```

---

## Known Limitations

### Lightning Layer
- ❌ Payment routing incomplete
- ❌ Wallet signing not implemented
- ✅ Read-only client works (channel validation, watchtower)

### Performance
- ⚠️ Not yet optimized for production
- ⚠️ Sync speed may be slower than expected

### Security
- ❌ No external security audit
- ❌ Use on testnet only
- ⚠️ Report issues privately to: security@dinero-coin.com

---

## Breaking Changes

### Consensus Protocol v3.0
This is a **major breaking change** from v2.x. Nodes running v3.0 cannot connect to v2.x nodes.

### Migration Path
There is no automated migration from v2.x to v3.0. Fresh sync required.

---

## Installation

### macOS

```bash
# Extract archive
tar -xzf DineroCoin-v3.0.0-alpha1-macos-arm64.tar.gz
cd DineroCoin-v3.0.0-alpha1-macos-arm64

# Run daemon
./bin/dinerod -daemon

# Check status
./bin/dinero-cli getblockchaininfo
```

### Linux

```bash
# Extract archive
tar -xzf DineroCoin-v3.0.0-alpha1-linux-x86_64.tar.gz
cd DineroCoin-v3.0.0-alpha1-linux-x86_64

# Run daemon
./bin/dinerod -daemon

# Check status
./bin/dinero-cli getblockchaininfo
```

### Windows

```powershell
# Extract zip
Expand-Archive DineroCoin-v3.0.0-alpha1-windows-x86_64.zip

# Run daemon
cd DineroCoin-v3.0.0-alpha1-windows-x86_64\bin
.\dinerod.exe -daemon

# Check status
.\dinero-cli.exe getblockchaininfo
```

---

## Testing & Feedback

### Testnet Deployment

We encourage testnet testing! To connect to the v3.0 testnet:

```bash
dinerod -testnet
```

### Reporting Issues

**Bug Reports:** https://github.com/Trucker2827/Dinero-Coin/issues

**Security Issues:** security@dinero-coin.com (private)

**Feature Requests:** https://github.com/Trucker2827/Dinero-Coin/discussions

---

## Next Steps

### v3.0.0-beta1 (Planned)
- Complete Lightning signature verification
- Fix bugs found in alpha testing
- Schedule external security audit
- Performance optimization

### v3.0.0 (Final)
- Complete external security audit
- Multi-week testnet deployment
- Production hardening
- Mainnet preparation

---

## Commit Log

See full changelog: [CHANGELOG.md](./CHANGELOG.md)

**Key Commits:**
- `76203015` - Wallet encryption fix + project status
- `baeae6ab` - Mobile profile enforcement
- `53f36c34` - Lightning integration merge
- `b65349f9` - Sync validation complete

---

## Credits

**Core Development:**
- DineroCoin Core Team

**Special Thanks:**
- Early testers
- Community contributors
- Utreexo research team

---

## ⚠️ Final Warning

**This is an ALPHA release. Do NOT use on mainnet.**

Use at your own risk. No warranty provided.

---

**Built:** 2026-01-XX
**Commit:** TBD (git rev-parse)
**Protocol:** v3.0
**Network:** Testnet recommended

For documentation, visit: https://github.com/Trucker2827/Dinero-Coin/tree/main/docs
```

---

## Post-Release Checklist

### Immediate (Day 3 evening)
- [ ] Release published on GitHub
- [ ] Announcement posted (Discussions, social media)
- [ ] Binaries tested on all platforms
- [ ] Documentation links verified

### Week 1
- [ ] Monitor issue tracker for alpha bugs
- [ ] Respond to early tester feedback
- [ ] Deploy testnet nodes
- [ ] Begin multi-node testing

### Week 2
- [ ] Triage reported bugs
- [ ] Fix critical issues found
- [ ] Plan beta timeline
- [ ] Schedule security audit

---

## Success Metrics

**How to measure alpha success:**

### Quantitative
- Number of downloads
- Number of GitHub stars/forks
- Issue reports filed
- Testnet nodes deployed
- Test coverage increase

### Qualitative
- Community excitement
- Developer adoption
- Tester feedback quality
- Exchange interest
- Media coverage

**Target for Beta:**
- 50+ downloads
- 10+ bug reports
- 5+ testnet nodes
- 0 data corruption bugs
- Positive community sentiment

---

## Risk Assessment

### Low Risk ⚠️
- Alpha labeled clearly (expectation management)
- Testnet only (no mainnet funds at risk)
- Documentation comprehensive (users know what to expect)
- Build system mature (proven in v2.x releases)

### Medium Risk ⚠️
- No security audit (unknown vulnerabilities may exist)
- Lightning incomplete (may confuse users)
- Performance not optimized (may disappoint users)

### Mitigation
- Prominent alpha warnings
- Clear known limitations section
- Active issue triage
- Quick response to critical bugs

---

## Conclusion

**DineroCoin is ready for v3.0.0-alpha1.**

**Next Actions:**
1. Review this document with maintainer
2. Execute Day 1-3 implementation roadmap
3. Tag v3.0.0-alpha1 when criteria met
4. Monitor and respond to feedback

**Timeline:** 2-3 days from start to alpha release

---

**Prepared By:** Claude Code
**Date:** 2026-01-11
**Status:** ✅ Ready for Implementation

**Supporting Documents:**
- `docs/RELEASE_READINESS_AUDIT.md` - Audit findings
- `docs/CI_PIPELINE_V3_ALPHA.md` - Build pipeline design
- `docs/SMOKE_TESTS_SPECIFICATION.md` - Testing specification
- `docs/V3_ALPHA1_CUT_CRITERIA.md` - Go/No-Go criteria
- `docs/PROJECT_STATUS.md` - Protocol completion status (existing)

**This document is the single source of truth for the v3.0.0-alpha1 release.**
