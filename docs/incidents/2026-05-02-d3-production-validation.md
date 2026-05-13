# Phase D.3 — Production Validation on Dell Tower

- **Date:** 2026-05-02
- **Severity:** N/A — validation record, not an incident
- **Tested commit:** `38b33aa29` (script in tree); fleet binary `0786b9e10`
- **Host:** Dell Tower (`tower@192.168.1.108`), real Linux, real running dinerod

## Why this record exists

Phase D.3 (`dinero-prepare-upgrade`) shipped with 11 unit-test
properties green on macOS dev. Mac unit tests verify the contract
shape; they do NOT verify that the script captures a real ELF from a
real `/proc/<pid>/exe` against a real running dinerod. This file
records the production-shaped validation that closes that gap.

Future archaeology — "did Phase D.3 actually work on Linux against
the live fleet, or just on the Mac CI box?" — answer: yes, here's
the date and the artifact.

## What was tested

### Test A — Capture while dinerod is running

Pre-state: `dinerod -datadir=/home/tower/.dinero` running as PID
1580888, commit `0786b9e10`. No prior captures in
`/home/tower/.dinero/binaries/`.

Command: `~/Dinero-Coin/share/scripts/dinero-prepare-upgrade
--datadir=/home/tower/.dinero`

Result:

| Property | Observed |
|---|---|
| Exit code | 0 |
| Captured file | `/home/tower/.dinero/binaries/dinerod.live-pre-0786b9e10-20260502T112854Z` |
| File mode | `0750` |
| File ownership | `tower:tower` |
| File size | 32,376,808 bytes |
| Captured binary `--version` | `dinerod 0786b9e10` |
| Commit match (captured vs running) | byte-equal |

### Test B — Capture while dinerod is stopped

Pre-state: `dinero-cli stop`, waited 9s for graceful exit, confirmed
no `dinerod` process via `pgrep -x dinerod`.

Command: same as Test A.

Result:

| Property | Observed |
|---|---|
| Exit code | 0 (non-fatal contract honored) |
| WARN line emitted | `[WARN] dinerod is not running (no PID found via pidof/pgrep)` |
| New file produced | No (binaries/ count stayed at 1, the file from Test A) |
| Existing capture from Test A | Untouched |

After Test B, dinerod was restarted via `nohup` to leave Dell in its
normal serving state.

## What this validates that the Mac unit test could not

1. `/proc/<pid>/exe` actually resolves to a usable copy of the live
   ELF, not a placeholder or read-failure.
2. The `pidof dinerod` discovery path works on real systemd-free
   Linux (Dell runs under nohup, not systemd).
3. The captured binary is bit-equivalent to the running one — same
   `--version` output, same commit hash, runs without error.
4. Mode `0750` is honored on a real Linux filesystem owned by a
   non-root operator (`tower`), not just on macOS HFS+.
5. The stopped-daemon non-fatal contract works against a real
   stop/start cycle, not a phantom-PID simulation.

## Artifact retained

The captured binary
`/home/tower/.dinero/binaries/dinerod.live-pre-0786b9e10-20260502T112854Z`
is the first production rollback artifact produced by the new
script. It stays on Dell as the literal "what to roll back to" for
the current `0786b9e10` deploy. Operators inspecting Dell will see
exactly what Phase D.3 promises: a binary at the contract path,
named after the running commit and a UTC timestamp.

## Out of scope for this validation

- `--exe=<path>` override path — covered by Mac unit test only.
- `--dry-run` — covered by Mac unit test only.
- Stale-backup preservation under heavy concurrency — covered by Mac
  unit test only.
- Behavior on systemd-managed servers (LA/VA/MO/CN) — deferred to
  Phase F (fleet migration), where each server gets its first
  capture as part of the migration smoke test.
