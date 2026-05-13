# Phase 3: Pre-Flight Checklist

**Status:** Ready for Review
**Purpose:** Gate-check before starting Phase 3 execution
**Updated:** 2026-01-07

---

## ⚠️ CRITICAL: DO NOT START PHASE 3 UNTIL ALL BOXES CHECKED

Phase 3 involves **refactoring** - changing structure without changing behavior. Starting prematurely risks:
- Invalidating test baselines
- Mixing bugs with architectural changes
- Difficult rollback scenarios
- Extended debugging during critical pre-launch period

---

## Section 1: Phase 2 Completion Gates

### 1.1 Testing Complete
- [ ] All Phase 2 tests passing (see `docs/TESTING.md`)
- [ ] Consensus engine validation complete
  - [ ] UTXO validation under stress
  - [ ] Utreexo proof generation/verification
  - [ ] Reorg handling (1-block, 10-block, 100-block)
  - [ ] Block validation edge cases
- [ ] Lightning integration validated
  - [ ] Channel lifecycle (open, update, close)
  - [ ] Force-close scenarios
  - [ ] HTLC enforcement
  - [ ] Watchtower integration
  - [ ] Cross-daemon RPC correctness
- [ ] Cross-cutting concerns tested
  - [ ] Fee estimation accuracy
  - [ ] RBF transaction replacement
  - [ ] Mempool management under load
  - [ ] Network synchronization

### 1.2 Mainnet Stability
- [ ] Mainnet running stable for **2+ weeks**
- [ ] No critical (P0) bugs in backlog
- [ ] No high-priority (P1) bugs affecting consensus or Lightning
- [ ] Utreexo audit complete and findings addressed
- [ ] No emergency patches required in last 2 weeks

### 1.3 Regression Test Baseline Established
- [ ] Full test suite runs and passes
- [ ] Baseline metrics captured:
  - [ ] Build time (minutes)
  - [ ] Binary size (MB)
  - [ ] Test execution time (seconds)
  - [ ] Peak memory usage (MB)
- [ ] Regression test suite documented
- [ ] CI/CD pipeline validates all commits

---

## Section 2: Phase 2.5 Completion (Option A)

### 2.1 Compile-Time Optional Lightning
- [x] `ENABLE_LIGHTNING` CMake option implemented ✅ (2026-01-07)
- [x] Lightning disabled by default ✅
- [x] Build with `ENABLE_LIGHTNING=OFF` succeeds ✅
- [ ] Build with `ENABLE_LIGHTNING=ON` attempted (may have errors - that's OK)
- [x] Documentation updated (`docs/BUILDING.md`) ✅

### 2.2 No Regressions from Option A
- [x] Default build (Lightning disabled) still passes all tests ✅
- [x] Binary size with Lightning disabled documented ✅
- [x] Build time with Lightning disabled measured ✅

---

## Section 3: Phase 3 Preparation

### 3.1 Documentation Complete
- [x] `docs/PHASE3_SYMBOL_AUDIT.md` created ✅ (2026-01-07)
  - [x] All symbols identified ✅
  - [x] Migration destinations planned ✅
  - [x] UTXO type mismatch documented ✅
- [x] `docs/PHASE3_COMMIT1_CHECKLIST.md` created ✅ (2026-01-07)
  - [x] Step-by-step execution plan ✅
  - [x] Rollback strategy defined ✅
  - [x] Acceptance criteria documented ✅
- [x] `docs/PHASE3_LIGHTNINGD_BINARY_LAYOUT.md` created ✅ (2026-01-07)
  - [x] Final binary structure planned ✅
  - [x] Dependency graph documented ✅
  - [x] Symbol verification script written ✅
- [ ] `docs/PHASE3_BUILD_DECOUPLING.md` reviewed
  - [ ] 10-commit plan understood
  - [ ] Risk assessment reviewed
  - [ ] Rollback points identified

### 3.2 Team Alignment
- [ ] Phase 3 scope reviewed with team
- [ ] All team members understand "no new features" during Phase 3
- [ ] Commit-by-commit review process agreed
- [ ] Emergency rollback procedure understood
- [ ] Code freeze period scheduled (if needed)

### 3.3 Branch Strategy
- [ ] Branch created: `phase3/lightning-build-decoupling`
- [ ] Branch protection rules configured
  - [ ] Require review before merge
  - [ ] Require CI/CD pass
  - [ ] Prevent force push
- [ ] Backup branch created: `phase2-final-state`

---

## Section 4: Environment Setup

### 4.1 Development Environment
- [ ] Clean working directory (`git status` empty)
- [ ] All uncommitted work stashed or committed
- [ ] Latest `main` branch pulled
- [ ] Dependencies up to date
  - [ ] CMake 3.20+
  - [ ] Clang or GCC with C++17
  - [ ] All third-party libraries current
- [ ] Disk space available (>10 GB free)

### 4.2 Build Verification
- [ ] Clean build from scratch succeeds:
  ```bash
  rm -rf build
  cmake -B build -DENABLE_LIGHTNING=OFF
  cmake --build build -j8
  ```
- [ ] All tests pass:
  ```bash
  cd build
  ctest --output-on-failure
  ```
- [ ] Binary sizes recorded:
  ```bash
  ls -lh build/bin/dinerod
  ls -lh build/lib/libdinero_core.a
  ```

### 4.3 Baseline Capture
- [ ] Create baseline snapshot:
  ```bash
  git tag -a phase2-baseline -m "Phase 2 final baseline"
  git push origin phase2-baseline
  ```
- [ ] Capture build metrics:
  ```bash
  time cmake --build build > build_time_baseline.txt
  ```
- [ ] Capture test metrics:
  ```bash
  time ctest --output-on-failure > test_time_baseline.txt
  ```

---

## Section 5: Risk Mitigation

### 5.1 Critical Path Analysis
- [ ] UTXO type fix identified as Commit 1 (blocker)
- [ ] Transaction::Serialize migration planned as Commit 3
- [ ] Final build-time decoupling planned as Commit 7
- [ ] Each commit has clear rollback strategy

### 5.2 Contingency Planning
- [ ] **If Phase 3 fails:** Rollback to `phase2-baseline` tag
- [ ] **If tests fail:** Revert last commit, investigate
- [ ] **If build breaks:** Use stash to preserve work, rollback
- [ ] **If deadline pressure:** Defer Phase 3, merge Phase 2.5 only

### 5.3 Communication Plan
- [ ] Stakeholders notified of Phase 3 start date
- [ ] Daily progress updates scheduled (during execution)
- [ ] Rollback criteria communicated to team
- [ ] External contributors notified (if applicable)

---

## Section 6: Execution Readiness

### 6.1 Time Allocation
- [ ] Week 1 allocated: Symbol inventory & interface design
- [ ] Week 2 allocated: Transaction primitives extraction
- [ ] Week 3 allocated: Lightning wallet stubs
- [ ] Week 4 allocated: Validation & cleanup
- [ ] Buffer time allocated: 1 week for unexpected issues

### 6.2 Resources Available
- [ ] Developer time committed (full-time for 4 weeks)
- [ ] Code review bandwidth confirmed
- [ ] CI/CD infrastructure ready
- [ ] Testing infrastructure ready (hardware, test data)

### 6.3 Success Criteria Agreed
- [ ] `lightningd` does not link `dinero_wallet` (final state)
- [ ] All runtime wallet access via gRPC (verified)
- [ ] No test failures vs. Phase 2 baseline
- [ ] Binary size reduction confirmed (~5-10 MB)
- [ ] Build time reduction confirmed (~10-15%)
- [ ] Symbol verification script passes
- [ ] Documentation updated

---

## Section 7: Final Go/No-Go Decision

**Review Date:** _______________

**Decision Criteria:**

| Criterion | Status | Blocker? | Notes |
|-----------|--------|----------|-------|
| All Phase 2 tests passing | ☐ Yes ☐ No | **YES** | |
| Mainnet stable 2+ weeks | ☐ Yes ☐ No | **YES** | |
| Utreexo audit complete | ☐ Yes ☐ No | **YES** | |
| No P0/P1 bugs | ☐ Yes ☐ No | **YES** | |
| Baseline established | ☐ Yes ☐ No | **YES** | |
| Documentation complete | ☐ Yes ☐ No | NO | |
| Team aligned | ☐ Yes ☐ No | NO | |
| Environment ready | ☐ Yes ☐ No | **YES** | |

**GO/NO-GO DECISION:**

- [ ] ✅ **GO** - All blockers cleared, proceed with Phase 3
- [ ] ❌ **NO-GO** - Defer Phase 3, document reasons:
  - Reason: _______________________________________________
  - New target date: ______________________________________

**Approvals:**

- [ ] Tech Lead: _________________ Date: _______
- [ ] QA Lead: __________________ Date: _______
- [ ] Project Manager: ___________ Date: _______

---

## Quick Reference: Starting Phase 3

**When all checkboxes are ticked:**

```bash
# Step 1: Create Phase 3 branch
git checkout -b phase3/lightning-build-decoupling

# Step 2: Start with Commit 1 (UTXO type fix)
# Follow: docs/PHASE3_COMMIT1_CHECKLIST.md

# Step 3: Execute commits incrementally
# Follow: docs/PHASE3_BUILD_DECOUPLING.md (Commits 2-10)

# Step 4: Final verification
scripts/verify_lightningd_independence.sh

# Step 5: Merge to main (after review)
git checkout main
git merge phase3/lightning-build-decoupling
```

---

## Appendix: Rollback Procedure

**If you need to abort Phase 3:**

```bash
# Option 1: Rollback entire Phase 3
git checkout main
git branch -D phase3/lightning-build-decoupling
git checkout phase2-baseline
git checkout -b phase2-recovery

# Option 2: Rollback specific commit
git checkout phase3/lightning-build-decoupling
git revert HEAD  # Revert last commit
git push origin phase3/lightning-build-decoupling

# Option 3: Reset to Phase 2 baseline
git reset --hard phase2-baseline
```

---

**Document Status:** Ready for Review
**Owner:** DineroCoin Core Team
**Next Review:** Before Phase 3 execution starts
**Last Updated:** 2026-01-07
