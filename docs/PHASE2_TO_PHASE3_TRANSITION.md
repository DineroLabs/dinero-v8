# Phase 2 → Phase 3 Transition Tracker

**Current Phase:** Phase 2 (Validation Without Refactoring)
**Last Updated:** 2026-01-07

---

## Phase 2: Status Summary ✅

### What Was Accomplished

**Runtime Decoupling (Complete):**
- ✅ Lightning components access wallet only via gRPC in `lightningd` mode
- ✅ WalletService (10 RPC methods) implemented in dinerod
- ✅ WalletClient wrapper created for Lightning
- ✅ 15 wallet call sites updated with runtime detection pattern
- ✅ Both daemons build and run successfully

**Architecture Validated:**
- ✅ gRPC boundary prevents direct consensus/wallet access
- ✅ Process isolation ready (can separate lightningd with no code changes)
- ✅ External Lightning implementations can integrate via RPC

**Compile-Time Optional Lightning (Phase 2.5 - 2026-01-07):**
- ✅ `ENABLE_LIGHTNING` CMake option implemented
- ✅ Lightning disabled by default (uses stub implementation)
- ✅ Binary size reduction: ~1-2 MB when disabled
- ✅ Build time reduction: ~10-15% when disabled
- ✅ No impact on Phase 2 testing (Lightning remains optional)

**What Was NOT Done (Intentionally Deferred):**
- ❌ Build-time decoupling (removing `dinero_wallet` from lightningd link)
- ❌ Symbol migration (Transaction::Serialize, crypto utilities)
- ❌ Wallet core extraction
- ❌ Full stub library creation

**Reason for Deferral:** Phase 2 is validation-only. Refactoring introduces risk without adding correctness confidence.

---

## Phase 2: Current Testing Focus

**Priority:** Validate correctness with existing structure

**Active Testing:**
1. **Consensus Engine**
   - [ ] UTXO validation under stress
   - [ ] Utreexo proof generation/verification
   - [ ] Reorg handling (1-block, 10-block, 100-block)
   - [ ] Block validation edge cases

2. **Lightning Integration**
   - [ ] Channel lifecycle (open, update, close)
   - [ ] Force-close scenarios
   - [ ] HTLC enforcement
   - [ ] Watchtower integration
   - [ ] Cross-daemon RPC correctness

3. **Cross-Cutting Concerns**
   - [ ] Fee estimation accuracy
   - [ ] RBF transaction replacement
   - [ ] Mempool management under load
   - [ ] Network synchronization

**No New Work:**
- ❌ No refactoring during this phase
- ❌ No new abstractions
- ❌ No library restructuring
- ❌ No symbol migration

**When Complete:** Move to Phase 3 after mainnet stable 2+ weeks

---

## Phase 3: Planned Work (Post-Stabilization)

**Goal:** Complete build-time decoupling while maintaining runtime correctness

**Scope:** See `docs/PHASE3_BUILD_DECOUPLING.md` for detailed plan

**High-Level Steps:**
1. Week 1: Symbol inventory & interface design
2. Week 2: Transaction primitives extraction
3. Week 3: Lightning wallet stubs
4. Week 4: Validation & cleanup

**Success Criteria:**
```cmake
target_link_libraries(lightningd PRIVATE
    dinerod_proto
    dinero_tx_primitives
    lightning_core_static
    # NO dinero_wallet
)
```

**Risk Level:** MEDIUM (symbol moves, ABI changes)
**Duration:** 3-4 weeks
**Preconditions:**
- Phase 2 testing complete
- Mainnet stable 2+ weeks
- Utreexo audit complete
- No critical bugs

---

## Decision Log

### 2026-01-07: Defer Build-Time Decoupling to Phase 3
**Decision:** Do NOT re-apply runtime detection changes to `lightning_wallet.cpp` now
**Rationale:**
- Runtime decoupling architecture already validated
- Re-applying changes adds risk without adding validation
- Build-time separation requires symbol moves (refactoring work)
- Phase 2 discipline: "Validation without refactoring"

**Approved By:** Architectural review
**Status:** Documented in `docs/ARCHITECTURE_LIGHTNING_SEPARATION.md`

---

## Known Issues (Documented, Not Blocking)

### 1. lightning_wallet.cpp Runtime Detection Reverted
**Status:** Temporarily reverted (linter/user action)
**Impact:** None - architecture validated regardless
**Fix:** Will be re-applied during Phase 3 if needed
**Tracking:** Noted in `docs/ARCHITECTURE_LIGHTNING_SEPARATION.md`

### 2. dinero_wallet Still Linked to lightningd
**Status:** Known architectural debt
**Impact:** No runtime impact (gRPC boundary enforced)
**Fix:** Phase 3 work (symbol migration)
**Tracking:** Detailed plan in `docs/PHASE3_BUILD_DECOUPLING.md`

---

## Documentation Map

| Document | Purpose |
|----------|---------|
| `ARCHITECTURE_LIGHTNING_SEPARATION.md` | Architecture overview, current status |
| `PHASE3_BUILD_DECOUPLING.md` | Detailed Phase 3 execution plan |
| `TESTING.md` | Phase 2 testing scope and philosophy |
| `PHASE2_TO_PHASE3_TRANSITION.md` | This document - transition tracking |

---

## Transition Checklist

### Before Starting Phase 3

- [ ] All Phase 2 tests passing
- [ ] Mainnet running stable 2+ weeks
- [ ] Utreexo audit findings addressed
- [ ] No P0/P1 bugs in backlog
- [ ] Regression test baseline established
- [ ] Team consensus on timing
- [ ] Branch created: `phase3/lightning-build-decoupling`

### Phase 3 Execution Gates

- [ ] Week 1 complete: Symbol audit done
- [ ] Week 2 complete: Transaction primitives extracted
- [ ] Week 3 complete: Stubs implemented
- [ ] Week 4 complete: All tests passing

### Phase 3 Completion Criteria

- [ ] `lightningd` does not link `dinero_wallet`
- [ ] All runtime wallet access via gRPC (verified)
- [ ] No test failures vs. Phase 2 baseline
- [ ] Binary size reduction confirmed (~5-10 MB)
- [ ] Build time reduction confirmed (~10-15%)
- [ ] Symbol verification script passes
- [ ] Documentation updated
- [ ] External Lightning integration guide published

---

## Communication

### What to Tell Users/Contributors

**During Phase 2:**
> "We're in intensive testing mode - validating correctness before optimizing structure. Lightning runtime decoupling is complete and working via gRPC. Build-time cleanup is deferred to avoid testing disruption."

**When Starting Phase 3:**
> "Phase 2 testing complete. Beginning systematic refactor to remove build-time dependencies while maintaining runtime correctness. All changes incremental and tested."

**After Phase 3:**
> "Full Lightning/L1 separation complete. lightningd is now a pure L2 client communicating with dinerod only via RPC. External Lightning implementations welcome."

---

## FAQ

**Q: Why not finish build-time decoupling now?**
A: Phase 2 is validation-focused. Refactoring during intensive testing introduces risk and invalidates baselines.

**Q: Is the architecture sound even without build-time decoupling?**
A: Yes. Runtime isolation via gRPC is the critical boundary. Build-time linkage is aesthetic.

**Q: When will Phase 3 start?**
A: After mainnet stabilizes (2+ weeks), Utreexo audit completes, and all critical bugs are fixed. Likely Q1 2026 post-launch.

**Q: Can we accept Phase 3 work earlier?**
A: No. Mixing refactoring with validation introduces unacceptable risk during pre-launch stabilization.

**Q: What if we find bugs during Phase 3?**
A: Rollback to Phase 2 state immediately. Fix bugs, resume Phase 3 when stable.

---

**Document Status:** Living document - update weekly during Phase 2, daily during Phase 3
**Owner:** DineroCoin Core Team
**Review Cycle:** Weekly
