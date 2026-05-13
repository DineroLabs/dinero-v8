# v3.0.0-alpha1 Cut Criteria

**Date:** 2026-01-11
**Purpose:** Define when v3.0.0-alpha1 is ready to ship
**Scope:** Go/No-Go decision framework

---

## Executive Summary

This document defines the **objective criteria** for cutting the v3.0.0-alpha1 pre-release. It provides a clear checklist to determine if the codebase is ready for public alpha testing.

**Guiding Principle:**
> "Alpha releases must be functionally complete and stable enough for developer testing, but need not be production-ready."

**Target Audience:**
- Developers
- Early testers
- Testnet operators
- Auditors (pre-audit evaluation)

**NOT Target Audience (for alpha):**
- Mainnet users
- Exchanges
- Production deployments

---

## Decision Framework

### ✅ SHIP IT

Cut v3.0.0-alpha1 when ALL of these are true:

1. ✅ **Protocol Complete** (non-negotiable)
2. ✅ **Builds Pass** on all platforms (non-negotiable)
3. ✅ **Core Tests Pass** (non-negotiable)
4. ✅ **Smoke Tests Pass** (non-negotiable)
5. ✅ **Documentation Updated** (non-negotiable)
6. ⚠️ **No Critical Bugs** (severity assessment)

### ❌ HOLD IT

Do NOT cut v3.0.0-alpha1 if ANY of these are true:

1. ❌ **Build failures** on any platform
2. ❌ **Test failures** in consensus/validation
3. ❌ **Known data corruption** bugs
4. ❌ **Consensus split** risk identified
5. ❌ **Security vulnerability** found (critical/high severity)

---

## Detailed Criteria

### Criterion 1: Protocol Complete ✅

**Definition:** All protocol features documented as "Phase 8-12 Complete" are implemented and committed.

**Verification Checklist:**
- [x] Phase 8: Stateless Validation (Utreexo)
  - Proof generation works
  - Proof verification works
  - Stateless/stateful modes coexist
- [x] Phase 9: Proof Network
  - Proof cache (LRU + TTL)
  - Proof router (multi-peer requests)
  - Proof gossip (INV_PROOF)
  - Proof compression (ZSTD)
- [x] Phase 10: Sync Validation
  - Resumable sync
  - Cache eviction handling
  - Network partition recovery
- [x] Phase 11: Lightning Integration
  - Read-only Lightning client
  - Channel validation with proofs
  - HTLC validation with proofs
  - Stateless watchtower
- [x] Phase 12: Mobile Profile
  - Compile-time enforcement
  - iOS compliance guards
  - Burst mode support

**Evidence:**
- `PROJECT_STATUS.md` marks all phases as ✅ COMPLETE
- Commits `baeae6ab` (mobile), `53f36c34` (lightning), `b65349f9` (sync)

**Status:** ✅ **PASS**

---

### Criterion 2: Builds Pass on All Platforms ✅

**Definition:** Release builds complete successfully on macOS, Linux, and Windows without errors.

**Verification Checklist:**
- [ ] macOS (arm64): `cmake --build build` succeeds
- [ ] macOS (x86_64): `cmake --build build` succeeds
- [ ] Linux (x86_64): `cmake --build build` succeeds
- [ ] Windows (x86_64): `cmake --build build` succeeds

**Command:**
```bash
cmake -S . -B build \
  -DDINERO_RELEASE=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=ON

cmake --build build --config Release --parallel
```

**Success Criteria:**
- Exit code 0
- No compilation errors
- No linker errors
- Binaries created: `dinerod`, `dinero-cli`

**Evidence Required:**
- CI workflow run showing all platforms green
- Local build logs from maintainer

**Status:** ⏳ **PENDING VERIFICATION**

**Action:** Run CI workflow, verify all platforms pass

---

### Criterion 3: Core Tests Pass ✅

**Definition:** Critical test suites pass without failures.

**Test Categories (by priority):**

#### P0: Consensus Tests (MUST PASS)
```bash
ctest --test-dir build -R "Ring" --output-on-failure
```
**Expected:** 65/65 Ring tests pass (100%)

#### P0: Stateless Validation Tests (MUST PASS)
```bash
ctest --test-dir build -R "stateless" --output-on-failure
```
**Expected:** All stateless validation tests pass

#### P1: Mobile Tests (SHOULD PASS)
```bash
ctest --test-dir build -R "mobile" --output-on-failure
```
**Expected:** T12.1-T12.9 pass (9 tests)

#### P1: Lightning Tests (SHOULD PASS)
```bash
ctest --test-dir build -R "lightning" --output-on-failure
```
**Expected:** Lightning client tests pass

#### P2: Wallet Tests (NICE TO PASS)
```bash
ctest --test-dir build -R "wallet" --output-on-failure
```
**Expected:** Wallet encryption tests pass

**Failure Tolerance:**
- ❌ P0 failures: **BLOCKER** (do not ship)
- ⚠️ P1 failures: Acceptable if documented, issue filed
- ✅ P2 failures: Acceptable if documented

**Evidence Required:**
- CI test results (CTest output)
- Test coverage report

**Status:** ⏳ **PENDING VERIFICATION**

**Action:** Enable tests in CI (`-DENABLE_TESTS=ON`), run full suite

---

### Criterion 4: Smoke Tests Pass ✅

**Definition:** Platform-specific smoke tests verify basic daemon functionality.

**Test Script:** `scripts/smoke-test.sh`

**Test Count:** 16 core tests per platform

**Verification Checklist:**
- [ ] macOS (arm64): `./scripts/smoke-test.sh` returns exit code 0
- [ ] macOS (x86_64): `./scripts/smoke-test.sh` returns exit code 0
- [ ] Linux (x86_64): `./scripts/smoke-test.sh` returns exit code 0
- [ ] Windows (x86_64): PowerShell smoke test passes

**Key Tests:**
1. Daemon starts successfully
2. RPC responds to commands
3. Blocks can be generated
4. Wallet operations work
5. Daemon shuts down cleanly

**Success Criteria:**
- All 16 tests pass on all platforms
- No crashes or hangs
- Clean shutdown (no orphaned processes)

**Evidence Required:**
- Smoke test output from CI
- No failures in smoke-test-logs artifacts

**Status:** ⏳ **PENDING IMPLEMENTATION**

**Action:** Create `scripts/smoke-test.sh`, integrate into CI

---

### Criterion 5: Documentation Updated ✅

**Definition:** User-facing documentation reflects v3.0.0 changes.

**Required Updates:**

#### 5.1: README.md
- [ ] Mention v3.0.0 protocol features (stateless, Lightning, mobile)
- [ ] Add alpha release warning
- [ ] Update build instructions if changed
- [ ] Link to PROJECT_STATUS.md

#### 5.2: CHANGELOG.md
- [ ] Create entry for v3.0.0-alpha1
- [ ] List breaking changes (if any)
- [ ] List new features
- [ ] List security notices

**Template:**
```markdown
## [3.0.0-alpha1] - 2026-01-XX

### ⚠️ ALPHA RELEASE
This is a pre-release version for testing only.
- No external security audit
- Use on testnet only
- Breaking changes expected

### Added
- Stateless validation (Utreexo)
- Proof network (cache, routing, gossip, compression)
- Lightning integration (read-only client, stateless watchtower)
- Mobile profile (compiler-enforced resource limits)
- Sync validation (resumable, cache-safe)

### Changed
- Consensus protocol v3.0 (breaking change)
- Block validation supports stateless mode

### Security
- Wallet encryption bug fixed (metadata persistence)
- No external audit completed (alpha only)
```

#### 5.3: Security Notice
- [ ] Create SECURITY.md (if not exists)
- [ ] Document alpha status
- [ ] Provide security contact: security@dinero-coin.com
- [ ] Warn against mainnet use

**Status:** ⏳ **PENDING**

**Action:** Update README, create CHANGELOG entry, add security notice

---

### Criterion 6: No Critical Bugs ⚠️

**Definition:** No known bugs that cause data loss, consensus splits, or security vulnerabilities.

**Bug Severity Classification:**

#### Critical (P0) - BLOCKER
- Data corruption
- Consensus split risk
- Security vulnerability (critical/high)
- Wallet fund loss

**Action:** FIX before alpha (blocker)

#### High (P1) - EVALUATE
- Crash on specific input
- Memory leak
- RPC failure
- Network partition

**Action:** Fix if easy, otherwise document and file issue

#### Medium (P2) - ACCEPTABLE
- UI glitches
- Performance issues
- Non-critical feature incomplete (e.g., Lightning payment routing)

**Action:** Document, file issue, defer to beta

#### Low (P3) - ACCEPTABLE
- Cosmetic issues
- Documentation gaps
- TODOs in code

**Action:** Ignore for alpha, address in beta

**Current Known Issues:**

1. **Lightning signature verification incomplete** (P2)
   - Impact: Lightning wallet can't sign transactions
   - Mitigation: Lightning read-only client works
   - Action: Document, defer to beta

2. **881 TODOs in codebase** (P3)
   - Impact: Code improvements needed
   - Mitigation: None block functionality
   - Action: Review consensus TODOs, defer others

3. **No external security audit** (P1)
   - Impact: Unknown vulnerabilities may exist
   - Mitigation: Alpha warning, testnet only
   - Action: Document prominently

**Verification Checklist:**
- [ ] Review open GitHub issues
- [ ] Review recent commits for bug fixes
- [ ] Check for "FIXME" comments in consensus code
- [ ] Query maintainers for known issues

**Status:** ⏳ **PENDING REVIEW**

**Action:** Audit issue tracker, assess severity of known bugs

---

## Supplementary Criteria (Nice to Have)

### S1: Binary Verification Passes

**Definition:** Release binaries have no unwanted dependencies.

**Tests:**
- macOS: No Homebrew dependencies (`otool -L`)
- Linux: No gRPC/protobuf dependencies (`ldd`)
- Windows: DLLs bundled correctly

**Status:** ⏳ **PENDING**

### S2: Reproducible Builds

**Definition:** Builds are deterministic (SOURCE_DATE_EPOCH respected).

**Verification:**
```bash
./scripts/hermetic-build.sh dinerod
sha256sum build/dinerod
```

**Status:** ⏳ **PENDING**

### S3: Performance Benchmarks

**Definition:** No performance regressions vs v2.x.

**Status:** ⏳ **DEFERRED TO BETA**

---

## Go/No-Go Checklist

**Use this checklist for the final cut decision:**

### Required (Must Be Green)
- [ ] Protocol complete (Phase 8-12)
- [ ] macOS build passes
- [ ] Linux build passes
- [ ] Windows build passes
- [ ] Ring tests pass (65/65)
- [ ] Stateless validation tests pass
- [ ] Smoke tests pass (macOS)
- [ ] Smoke tests pass (Linux)
- [ ] Smoke tests pass (Windows)
- [ ] README.md updated
- [ ] CHANGELOG.md entry created
- [ ] No P0 bugs

### Recommended (Should Be Green)
- [ ] Mobile tests pass (T12.1-T12.9)
- [ ] Lightning tests pass
- [ ] Binary verification passes (macOS)
- [ ] Binary verification passes (Linux)
- [ ] Security notice added
- [ ] No P1 bugs (or documented)

### Optional (Nice to Have)
- [ ] Wallet tests pass
- [ ] Reproducible builds verified
- [ ] Performance benchmarks run
- [ ] GUI builds (dinero-qt)

---

## Decision Matrix

| Required Criteria Met | Recommended Criteria Met | Decision |
|-----------------------|--------------------------|----------|
| ✅ All | ✅ All | **✅ SHIP IT** (ideal) |
| ✅ All | ⚠️ Some | **✅ SHIP IT** (acceptable) |
| ✅ All | ❌ None | **⚠️ EVALUATE** (case-by-case) |
| ⚠️ Some | - | **❌ HOLD** (fix required) |
| ❌ None | - | **❌ ABORT** (major issues) |

---

## Release Timeline (Example)

### Day 0: Pre-Release Preparation
- [ ] Update documentation (README, CHANGELOG)
- [ ] Create smoke test script
- [ ] Enable tests in CI
- [ ] Run local builds on all platforms

### Day 1: CI Verification
- [ ] Push changes to trigger CI
- [ ] Monitor GitHub Actions runs
- [ ] Fix any platform-specific issues
- [ ] Verify all tests pass

### Day 2: Final Review
- [ ] Review Go/No-Go checklist
- [ ] Assess known bugs
- [ ] Get maintainer sign-off
- [ ] Prepare release notes

### Day 3: Tag & Release
- [ ] Tag `v3.0.0-alpha1` on main
- [ ] Wait for CI to build artifacts
- [ ] Create GitHub pre-release
- [ ] Announce to community

---

## Rollback Plan

**If critical issues are found after tagging:**

1. **Immediately:**
   - Mark GitHub release as "This release contains critical issues"
   - Pin issue to repository
   - Notify users via social media

2. **Within 24 hours:**
   - Fix critical issue
   - Cut v3.0.0-alpha2 with fix
   - Document what changed

3. **Communication:**
   - Post-mortem: What went wrong?
   - Process improvement: What checks were missed?
   - Update cut criteria document

---

## Post-Release Criteria (For Beta)

**v3.0.0-beta1 requires:**

- ✅ Alpha release tested on testnet (2+ weeks)
- ✅ Critical bugs from alpha fixed
- ✅ Lightning signature verification complete
- ✅ Security audit scheduled or in progress
- ✅ Performance benchmarks show acceptable results
- ✅ Multi-node testnet deployed

**v3.0.0 (final) requires:**

- ✅ Beta tested on testnet (4+ weeks)
- ✅ External security audit COMPLETE
- ✅ All P0/P1 bugs fixed
- ✅ Production hardening complete
- ✅ Mainnet deployment plan finalized

---

## Communication Plan

### Alpha Announcement Template

```markdown
# DineroCoin v3.0.0-alpha1 Released 🚀

We're excited to announce the first alpha release of DineroCoin v3.0!

## ⚠️ Alpha Release Notice
This is a **pre-release** version for testing purposes.
- ❌ NOT for production use
- ❌ NOT audited by external security researchers
- ✅ Recommended for testnet use only
- ✅ Feedback welcome!

## What's New in v3.0
- **Stateless Validation** - Full nodes without UTXO database (Utreexo)
- **Proof Network** - Efficient proof distribution (cache, gossip, compression)
- **Lightning Integration** - Stateless watchtower, proof-based channel validation
- **Mobile Profile** - Compiler-enforced resource limits for iOS/Android

## Download
- macOS (arm64): [Download](...)
- macOS (x86_64): [Download](...)
- Linux (x86_64): [Download](...)
- Windows (x86_64): [Download](...)

## Verification
```bash
shasum -a 256 -c DineroCoin-v3.0.0-alpha1-*.sha256
```

## Known Limitations
- Lightning payment routing incomplete (read-only mode only)
- No external security audit (scheduled for beta)
- Performance not yet optimized

## Feedback
Please report issues at: https://github.com/Trucker2827/Dinero-Coin/issues

## Security
Report security issues privately to: security@dinero-coin.com

---

**Do NOT use this release on mainnet. Testnet only.**
```

---

## Conclusion

**Cut v3.0.0-alpha1 when:**

1. ✅ All Required criteria met
2. ✅ Most Recommended criteria met
3. ⚠️ Known issues documented

**Do NOT cut v3.0.0-alpha1 if:**

1. ❌ Any build failures
2. ❌ Ring tests fail
3. ❌ P0 bugs exist

**Next Steps After Alpha:**

1. Collect feedback from early testers
2. Fix bugs found in alpha testing
3. Complete Lightning signature verification
4. Schedule security audit
5. Prepare v3.0.0-beta1

---

**Document Date:** 2026-01-11
**Author:** Claude Code
**Status:** Ready for use

**Use this document for the final v3.0.0-alpha1 cut decision.**
