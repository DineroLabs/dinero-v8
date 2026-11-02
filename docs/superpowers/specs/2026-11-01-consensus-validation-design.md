# Consensus validation — option A: CV1 header-reach gate (rev 2, CV1-only public draft)

**Status:** the owner chose option A on 2026-11-01. This public draft is scoped to **CV1 header reach only**
(rev 2, 2026-11-02); the live-rule inventory and caller/duplicate-object gates are tracked privately and are not
part of this PR. Report-only, **do not merge**.
**Baseline:** dinero-main c4940d86c (the same baseline as the mempool project). The survey is
`2026-11-01-consensus-validation-survey.md`.
**Sibling projects:** wallet/node separation (#860, leg 1a = storage-free `dinero_consensus` library) and mempool
separation. This project depends on leg 1a and does not overlap it.

## 1. Goal
Make the `dinero_consensus` / `dinero_consensus_core` **header boundary** measurable without changing behavior.
CV1 records the baseline, reports changes and fails only on new regressions. No gate here moves code.

## 2. Non-goals
- No edits to production sources, CMake lists, frozen headers or tests owned by other projects.
- No change in validation behavior.
- No chainparams/`Params()` work (deferred).
- No reuse of #860 gate scripts by import (they aren't on dinero-main yet). The tool here is self-contained;
  it can be unified once #860 lands.

## 3. Gate (OFF and ON; Python, stdlib only; pytest in `scripts/ci/consensus-separation/tests/`)

| Gate | What it measures | Input | Fails on |
|---|---|---|---|
| **CV1 header reach** | every header the objects of `dinero_consensus` and `dinero_consensus_core` reach (preprocessor `-MM -MG`), in forbidden families: storage, daemon, p2p, net, rpc, wallet, mining, indexing, mempool, node, chainstate, core, plus RocksDB and sqlite3 | compile_commands.json | a forbidden reach from a source not in the baseline; any unresolved dependency; a failed preprocess; a failed positive control |

**Bucket rule (as in the mempool D1):** CV1 reaches from sources that leg 1a moves or changes (coins_db,
coins_view_cache, global_utxo_set_impl, utxo_position_index, addr_codec, tx_validation) are bucket A: reported,
never failing, and expected to disappear as leg 1a lands. The baseline shrinks as they go, and a shrink is a
reported improvement, not a failure. `include/mining/header_layout.h` and `include/storage/tip_info.h` are
allowlisted as pure (survey section 2); moving them is a follow-up.

**Self-tests:** CV1 has a positive control (a known violation is found) and a neuter test (an injected regression
fails the gate). CV1's positive control is coins_db.cpp -> rocksdb.

## 4. CI
A separate workflow, `consensus-gates.yml`, modeled on the hermetic POSIX/Windows separation jobs: it configures
only and runs CV1 plus pytest. No Actions cache save and no artifact upload (owner no-upload rule). Logs and
step summaries are allowed. Pushing to the draft PR needs owner approval of the exact SHA (same as mempool D3).

## 5. Stop points
1. Before pushing anything (the draft PR with the workflow).
2. If the gate needs an edit to a shared file (root/consensus/wallet CMake, tests/CMakeLists.txt): stop and report.
