# Ring 4 — Phase 4h.1
# Production Persistence Design (RocksDB)

**Status**: 📐 DESIGN ONLY
**Precondition**: Ring 4g SEALED (161/161 tests passing)
**Implementation**: ❌ NOT STARTED
**Date**: 2026-01-03

---

## 1. Purpose of Phase 4h

Phase 4h transitions mining persistence from **abstract simulation** (Phase 4g) to **production-grade disk-backed storage**, while preserving all proven properties.

**Critical rule**:
> Phase 4h must satisfy MR1–MR5 **unchanged**.
> No new semantics are introduced.
> No weakening of guarantees is allowed.

---

## 2. What Phase 4h Is — and Is Not

### Phase 4h **IS**:
- Production persistence implementation
- Real disk I/O
- Crash-safe storage
- RocksDB-backed
- Verified against Ring 4g property tests

### Phase 4h **IS NOT**:
- A redesign of mining logic
- A rewrite of state machines
- A relaxation of safety or determinism
- A performance-optimization phase (yet)

---

## 3. Persistence Responsibility Boundary

`ProductionPersistenceStore` is **responsible for**:
- Persisting mining state snapshots
- Recovering state after restart
- Detecting corruption
- Failing conservatively
- Preserving determinism

It is **not responsible for**:
- Consensus validation
- Block validation
- Subsidy calculation
- Mining control flow

---

## 4. Persistence Model (Production)

### 4.1 Stored State (Authoritative)

The persisted snapshot must contain **only consensus-relevant mining state**:

| Category | Examples |
|----------|----------|
| Chain context | tip height, tip hash |
| Mining progress | blocks found, templates created |
| Subsidy accounting | total subsidy minted |
| Template state | active template metadata |
| Deterministic counters | checkpoint version |

**Explicitly excluded**:
- Timestamps
- Wall-clock data
- Thread counts
- Performance metrics
- Any entropy source

This mirrors Phase 4g's abstract state.

---

## 5. Storage Backend: RocksDB

### Why RocksDB
- Proven crash safety
- Atomic WriteBatch support
- Checksums
- Fast recovery
- Battle-tested in blockchain systems

### Required Features
- Single column family (initially)
- Atomic batch writes
- Sync-on-write enabled
- Checksums enabled
- WAL enabled

---

## 6. Atomicity & Crash Safety

### Required Guarantees

Persistence must satisfy:

| Scenario | Required Outcome |
|----------|------------------|
| Clean shutdown | Exact state restored |
| Crash mid-write | Last valid snapshot restored OR empty |
| Power loss | No partial state exposed |
| Corruption | Recovery fails safely |

**Forbidden outcomes**:
- Partial state
- Mixed old/new state
- Duplicate subsidy
- Duplicate block height

These are enforced by **MR2 + MR3**.

---

## 7. Write Semantics

### Checkpoint Model
- Persistence uses **full snapshot overwrite**
- No incremental or append-based semantics
- Each persist produces a new logical checkpoint
- Checkpoint version monotonically increases

### Atomic Write Pattern
```
WriteBatch:
  - snapshot_blob
  - snapshot_checksum
  - snapshot_version
  - schema_version

Commit must be all-or-nothing.
```

---

## 8. Recovery Semantics

### Recovery Algorithm (Conceptual)
1. Open RocksDB
2. Read snapshot blob
3. Verify checksum
4. Validate schema version
5. Deserialize state
6. Validate invariants
7. If any step fails → conservative recovery

### Conservative Recovery

**Allowed**:
- Return last valid snapshot
- Return empty initial state

**Forbidden**:
- Returning partially valid state
- Guessing or repairing silently

This directly satisfies **MR3 + MR4**.

---

## 9. Determinism Requirements (MR5)

Persistence must **not introduce entropy**.

Therefore:
- Serialization order must be deterministic
- No unordered containers without canonical ordering
- No timestamps
- No random UUIDs
- No OS-dependent metadata

**Same seed + same actions + same crashes**
→ **identical recovered trace**

This is non-negotiable.

---

## 10. Versioning & Compatibility

### Schema Versioning
- Explicit `schema_version` field required
- Versioned deserialization
- Unknown versions → fail safely
- No silent migrations in Phase 4h.

---

## 11. Failure Handling Strategy

| Failure Type | Behavior |
|--------------|----------|
| Missing DB | Start empty |
| Corrupt DB | Fail safe → empty |
| Partial write | Roll back |
| Unsupported version | Refuse recovery |

Mining must be able to continue safely after recovery.

---

## 12. Integration Points (Not Implemented Yet)

Phase 4h will eventually integrate with:
- MiningManager lifecycle
- Controlled persist triggers
- Restart recovery path

⚠️ **None of this is implemented in Phase 4h.1**

---

## 13. Testing Strategy (Locked)

Phase 4h must pass **exactly the same tests** as Phase 4g:
- MR1–MR5 property tests
- Determinism tests
- Crash/restart scenarios

**No test changes allowed.**

If a test fails, **the implementation is wrong**.

---

## 14. Phase 4h Sub-Phases (Planned)

| Phase | Scope |
|-------|-------|
| 4h.1 | Design (this document) |
| 4h.2 | ProductionPersistenceStore skeleton |
| 4h.3 | RocksDB integration |
| 4h.4 | MR1–MR5 execution on real storage |
| 4h.5 | OS-level crash testing |
| 4h.6 | Performance & tuning |

---

## 15. Exit Criteria for Phase 4h.1

Phase 4h.1 is complete when:

- ✅ Persistence responsibilities defined
- ✅ Crash semantics specified
- ✅ Determinism constraints documented
- ✅ MR1–MR5 mapped to production behavior
- ✅ No code written

---

## 16. Final Notes

> Ring 4h exists because Ring 4g exists.

Most projects discover persistence bugs **after users lose funds**.

You are discovering them **before code exists**.

That is the correct order.

---

## Phase 4h.1 Status

**📐 DESIGN COMPLETE — READY FOR REVIEW**
