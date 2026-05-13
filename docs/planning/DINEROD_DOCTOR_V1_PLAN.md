# Dinerod Doctor v1 Plan

**Date**: 2026-02-13  
**Status**: Planning (no implementation in this document)  
**Owner**: Core daemon/ops reliability  
**Goal**: Ship a deterministic, operator-safe `dinerod doctor` command that lowers node operations friction.

---

## 1. Executive Summary

`dinerod doctor` should be treated as product infrastructure, not a debug script:

1. Read-only by default.
2. Deterministic machine contract (`exit codes`, stable check IDs, versioned JSON schema).
3. Actionable fix plans (risk, downtime, preconditions, exact commands/steps).
4. Two modes:
   - `--quick` (seconds)
   - `--deep` (minutes, budgeted)

This is a trust and adoption feature for exchanges/mining ops.

---

## 2. Scope and Non-Goals

### In Scope (v1)

1. `dinerod doctor` command entry point.
2. Check registry with metadata and timeout budgets.
3. Structured results + human output + `--json`.
4. Deterministic exit codes.
5. Initial check set (8-12 checks).
6. Fix-plan objects and safe/apply policy.

### Out of Scope (v1)

1. Broad auto-repair of chain DB state.
2. Reindex/prune-reset automation.
3. Consensus parameter or policy mutation.
4. Replacing existing RPC surface.

---

## 3. Contracts to Freeze Before Coding

These are ABI/API-like contracts for operators and automation. Treat them as stable.

### 3.1 Exit Codes

1. `0`: healthy
2. `1`: warnings only
3. `2`: critical findings (operator action required)
4. `3`: doctor internal error (incomplete run, bug, or unrecoverable framework failure)

### 3.2 Modes

1. `--quick` (default): bounded checks, target under 30s on normal hardware.
2. `--deep`: expanded checks, target under 10 minutes with per-check time budgets.

### 3.3 Check ID Convention

Format: `domain.subdomain.check_name`  
Examples:

1. `storage.disk_space`
2. `db.tip_consistency`
3. `p2p.dns_seeds.resolve`
4. `inv.supply_bounds`

Check IDs are stable contracts and must not be renamed after release.

### 3.4 JSON Contract

1. `--json` emits machine interface.
2. Top-level `schema_version` required.
3. Additive-only evolution for new fields.
4. Human output is for people, not parsers.

### 3.5 Mutation Guardrails

Read-only default.  
Mutation requires:

1. `--apply-safe-fixes`
2. explicit fix selection (`--fix <id>` one or more), or explicit batch override (`--yes-i-know-what-im-doing`)

No destructive fix is auto-applied in v1.

---

## 4. Architecture

### 4.1 Core Types

### `DoctorCheckMetadata`

1. `id`
2. `severity_default` (`INFO|WARN|CRIT`)
3. `mode` (`QUICK|DEEP|BOTH`)
4. `risk` (`NONE|LOW|MED|HIGH`) for fixes
5. `dependencies` (check IDs)
6. `timeout_budget_ms`

### `DoctorCheckResult`

1. `id`
2. `status` (`PASS|WARN|CRIT|ERROR|SKIP`)
3. `message`
4. `evidence` (structured key/value object)
5. `fix_plan[]` (0..N `FixAction`)
6. `duration_ms`

### `FixAction`

1. `id`
2. `safe_to_apply`
3. `risk`
4. `expected_downtime`
5. `preconditions[]`
6. `steps[]` (exact deterministic steps/commands)
7. `rollback_notes` (optional)

### 4.2 Runtime Components

1. `DoctorRegistry`: register checks + metadata.
2. `DoctorRunner`: deterministic execution order, dependency handling, timeout enforcement.
3. `DoctorContext`: read-only access to node services and environment probes.
4. `DoctorRenderer`: human output.
5. `DoctorJsonEmitter`: schema-versioned JSON output.

### 4.3 Determinism Rules

1. Fixed check execution order (topological + lexical tie-break).
2. Stable field ordering in JSON output.
3. Explicit timeout handling (`ERROR` with reason, not silent skip).
4. No direct `stdout` writes from checks; all output routed through structured results.

---

## 5. v1 Check Set (Initial 10)

### Storage

1. `storage.disk_space` (`BOTH`)
2. `storage.permissions` (`BOTH`)
3. `storage.fsync_latency.sample` (`BOTH`, larger sample in deep)

### Database

4. `db.sqlite.quick_check` (`QUICK`)
5. `db.tip_consistency` (`QUICK`)
6. `db.rocksdb.checksum_sample` (`DEEP`)

### Mempool

7. `mempool.snapshot_sanity` (`QUICK`)

### P2P

8. `p2p.bind_listen` (`QUICK`)
9. `p2p.dns_seeds.resolve` (`QUICK`)

### Invariants

10. `inv.supply_bounds` (`QUICK`)
11. `inv.chainstate_continuity` (`QUICK`)

Note: if schedule pressure appears, merge `inv.chainstate_continuity` into `db.tip_consistency` for v1 and keep total at 10 checks.

---

## 6. Safe Fix Policy (v1)

### Allowed for `--apply-safe-fixes` (strict)

1. Create missing non-critical runtime directories.
2. Correct clearly invalid permissions on node-owned operational paths.
3. Remove doctor-generated temporary files.

### Never auto-apply in v1

1. Reindex
2. Prune reset
3. RocksDB repair/compaction
4. Deleting MANIFEST/WAL/chain DB files
5. Anything requiring daemon stop/restart unless user performs explicit manual workflow

---

## 7. CLI Surface (v1)

1. `dinerod doctor` (quick, read-only)
2. `dinerod doctor --deep`
3. `dinerod doctor --json`
4. `dinerod doctor --json --output <path>`
5. `dinerod doctor --list-checks`
6. `dinerod doctor --checks <glob,...>`
7. `dinerod doctor --explain <check_id>`
8. `dinerod doctor --apply-safe-fixes --fix <fix_id>`
9. `dinerod doctor --apply-safe-fixes --yes-i-know-what-im-doing`

---

## 8. JSON Schema v1 (Top-Level)

```json
{
  "schema_version": "1.0",
  "node_version": "vX.Y.Z",
  "network": "mainnet",
  "timestamp": "2026-02-13T18:04:12Z",
  "mode": "quick",
  "exit_code": 1,
  "summary": { "critical": 0, "warnings": 3, "info": 9 },
  "checks": []
}
```

Each `checks[]` element includes `id`, `status`, `message`, `evidence`, and `fix_plan[]`.

---

## 9. Implementation Phases

### Phase 0: Contract Freeze (planning gate)

1. Approve this plan.
2. Lock exit codes, check ID naming convention, and JSON top-level schema.
3. Lock safe-fix policy boundaries.

### Phase 1: Framework Skeleton

1. Command wiring and mode parsing.
2. Check registry and metadata model.
3. Runner with timeout and dependency handling.
4. Human output + JSON emitter.

### Phase 2: v1 Quick Checks

1. Implement quick-path checks from section 5.
2. Add per-check evidence payloads.
3. Add fix-plan generation (suggest-only first).

### Phase 3: Safe Apply

1. Implement `--apply-safe-fixes` flow.
2. Enforce preconditions and dry-run preview.
3. Re-run affected checks post-apply.

### Phase 4: Deep Checks + Hardening

1. Add deep-path checks with budget controls.
2. Add stress/fault-injection tests.
3. Finalize operator docs and runbook examples.

---

## 10. Testing and Acceptance Criteria

### Required Tests

1. Unit tests for registry ordering, dependency handling, and timeout behavior.
2. Contract tests for exit codes.
3. JSON schema snapshot tests (`schema_version=1.0`).
4. Golden tests for deterministic check ordering.
5. Integration tests with synthetic unhealthy fixtures.
6. Safe-fix tests with precondition failures and partial success behavior.

### Acceptance Criteria (v1 ship gate)

1. Quick mode passes under 30s on reference environment.
2. Deep mode bounded by configured check budgets.
3. `--json` output validates against v1 schema in CI.
4. No auto-applied destructive action exists in code path.
5. Exit code behavior is deterministic and documented.

---

## 11. Risks and Mitigations

1. **Doctor drift into ad-hoc scripts**
   - Mitigation: enforce registry-only checks and metadata completeness.
2. **Schema churn breaks operator automation**
   - Mitigation: versioned schema, additive-only changes.
3. **False confidence from shallow checks**
   - Mitigation: explicit quick/deep semantics and per-check evidence.
4. **Unsafe auto-fix overreach**
   - Mitigation: narrow safe-fix allowlist, strict preconditions, no destructive actions.

---

## 12. Open Decisions (Before Implementation Starts)

1. Exact command location: daemon binary subcommand vs standalone admin binary.
2. Final list of v1 checks (10 vs 11).
3. Whether `--apply-safe-fixes` supports multi-fix atomicity or best-effort order.
4. Whether `--json` should include per-check `started_at`/`finished_at` timestamps.
5. How to expose doctor result summaries via RPC (v1 or v1.1).

---

## 13. Recommended Next Step

Hold a short contract review and approve/adjust:

1. exit code contract
2. check ID namespace
3. JSON v1 top-level schema
4. safe-fix boundaries

After that, implementation begins from Phase 1 with no contract churn.
