# Dinero Core v1.0.0 Release Plan

## 🎯 **Goal**

**Dinero Core v1.0.0** = First stable modular release

---

## 📋 **Prerequisites (Current Status: 50% Complete)**

| Step              | Status        | Completion        |
| ----------------- | ------------- | ----------------- |
| IConsensusEngine  | ⛔ Not Started | 0%                |
| Metrics per Miner | 🟡 Partial    | 70%               |
| Context Tests     | ✅ Done        | 100%              |
| Core v1 Docs      | 🟡 Partial    | 60%               |
| **Overall**       | —             | **50% Complete**  |

---

## 🚀 **Execution Plan**

### **Phase 1: Add Metrics Labels** (1-2 days)
**Quick win, visible in dashboards**

**Tasks**:
- [ ] Add label parameter support to `MetricsRegistry` methods
- [ ] Update `SetMiningHashrate()`, `SetMiningThreads()`, etc. to accept labels
- [ ] Uncomment per-miner label code in `MiningService::UpdateTelemetry()`
- [ ] Test multi-miner scenarios
- [ ] Verify Prometheus/JSON export shows per-miner metrics

**Deliverable**: Per-miner metrics working in dashboards

---

### **Phase 2: Implement IConsensusEngine** (2-3 days)
**Unlocks modular consensus**

**Tasks**:
- [ ] Create `include/consensus/iconsensus_engine.h` interface
- [ ] Define abstract methods: `ValidateBlock()`, `CreateNewBlock()`, `GetName()`
- [ ] Create `src/consensus/pow_consensus_engine.cpp` implementation
- [ ] Extract PoW logic from `Mining` class into `PowConsensusEngine`
- [ ] Update `MiningService` to use `IConsensusEngine` instead of direct `Mining`
- [ ] Add tests for consensus swapping
- [ ] Verify all existing tests pass

**Deliverable**: Modular consensus architecture

---

### **Phase 3: Consolidate Docs** (1 day)
**Finalize v1 documentation**

**Tasks**:
- [ ] Create `docs/DINERO_CORE_V1_ARCHITECTURE.md`
- [ ] Consolidate content from existing architecture docs
- [ ] Add service lifecycle diagram
- [ ] Document extension guidelines:
  - How to add a new consensus engine
  - How to add a new service
  - How to add a new RPC method
- [ ] Cross-reference all architecture docs
- [ ] Add "Dinero Core v1 Architecture" branding

**Deliverable**: Complete v1 architecture documentation

---

### **Phase 4: Publish v1 Tag** (1 day)
**Dinero Core v1.0.0 = first stable modular release**

**Tasks**:
- [ ] Update version numbers in codebase
- [ ] Create `RELEASE_NOTES_v1.0.0.md`
- [ ] Document all v1.0.0 features and improvements
- [ ] Run final test suite
- [ ] Create git tag: `git tag -a v1.0.0 -m "Dinero Core v1.0.0: First stable modular release"`
- [ ] Push tag: `git push origin v1.0.0`
- [ ] Prepare release announcement

**Deliverable**: `v1.0.0` tag published

---

## 📅 **Timeline**

| Phase | Duration | Cumulative | Deliverable |
|-------|----------|------------|-------------|
| 1️⃣ Metrics Labels | 1-2 days | 1-2 days | Per-miner metrics |
| 2️⃣ IConsensusEngine | 2-3 days | 3-5 days | Modular consensus |
| 3️⃣ Consolidate Docs | 1 day | 4-6 days | v1 Architecture doc |
| 4️⃣ Publish v1 Tag | 1 day | 5-7 days | v1.0.0 release |
| **Total** | **5-7 days** | — | **Dinero Core v1.0.0** |

---

## ✅ **Success Criteria**

### **Phase 1: Metrics Labels** ✅
- [ ] `MetricsRegistry` supports label parameters
- [ ] Per-miner metrics visible in Prometheus/JSON export
- [ ] Dashboard shows separate miner instances
- [ ] All existing metrics still work

### **Phase 2: IConsensusEngine** ✅
- [ ] `IConsensusEngine` interface defined and documented
- [ ] `PowConsensusEngine` implementation complete
- [ ] `MiningService` uses `IConsensusEngine`
- [ ] All tests pass with new architecture
- [ ] Consensus logic decoupled from mining

### **Phase 3: Consolidate Docs** ✅
- [ ] `docs/DINERO_CORE_V1_ARCHITECTURE.md` created
- [ ] Service lifecycle diagram included
- [ ] Extension guidelines documented
- [ ] All architecture docs cross-referenced
- [ ] "v1" branding consistent

### **Phase 4: Publish v1 Tag** ✅
- [ ] Release notes prepared
- [ ] Version numbers updated
- [ ] `v1.0.0` tag created and pushed
- [ ] Release announcement ready
- [ ] All tests passing

---

## 🏆 **v1.0.0 Release Definition**

**Dinero Core v1.0.0** represents the first stable modular release with:

✅ **100% Context-Driven Architecture**
- No globals in core code
- Full dependency injection via `DaemonContext`
- Service-oriented design

✅ **Modular Consensus Engine**
- `IConsensusEngine` interface for PoW/PoS/hybrid swapping
- `PowConsensusEngine` implementation
- Testable consensus logic

✅ **Per-Miner Metrics & Monitoring**
- Label support in `MetricsRegistry`
- Per-miner dashboards
- Real-time telemetry

✅ **Comprehensive Architecture Documentation**
- Unified `DINERO_CORE_V1_ARCHITECTURE.md`
- Service lifecycle diagrams
- Extension guidelines

✅ **Production-Ready Codebase**
- All critical bugs fixed
- Context-aware unit tests
- Clean three-layer data model

---

## 🎉 **Milestone Achievement**

**Status**: **50% → 100%** (5-7 days remaining)

**Current**: Week 6 complete (architecture migration done)

**Next**: Complete Next Evolution Steps → v1.0.0 release

**Timeline**: **5-7 days** to v1.0.0

---

**See Also**:
- `docs/NEXT_EVOLUTION_STEPS_STATUS.md` - Detailed status
- `docs/NEXT_EVOLUTION_STEPS_QUICK_REF.md` - Quick reference

