# MyNodeDashboard Phase 1 (MVP) — Automated sanity log

**Branch:** `feature/my-node-dashboard-mvp`
**Submodules:** verified pinned to dinero-main's recorded commits (rocksdb v9.9.3, secp256k1-zkp 42e75b61, snappy 1.2.2-4-g6f99459, zstd v1.4.7-2898-g98d2b90e) — initial build was against drifted submodule HEADs inherited from worktree-add; re-verified after `git submodule update --init --recursive`.

## Results (post-submodule-sync)

| Check | Result |
|---|---|
| Full `dinero-qt` app build | `[100%] Built target dinero-qt` (macOS bundle, ad-hoc signed dev build, code-sign valid on disk) |
| ctest (4 suites) | 4/4 pass — NodePoller, IdentitySection, NetworkSection, PeersSection (17 internal tests) |
| App launch (clean isolated datadir) | Starts, alive >5s, exits cleanly on SIGTERM |

## Pending human verification (per plan Task 12 Step 3)

- Cmd+K opens panel from the right within a frame (no jank)
- Default tab is Dashboard
- Live identity/network/peers data renders against a running daemon
- AI tab still works (configScreen or chatScreen depending on saved API key state)
- Cmd+K toggles closed; reopening restores last-active tab
- Daemon stop → **IdentitySection** shows "● UNREACHABLE" within ~15s; daemon restart → green within ~10s

## Phase 1 scope note: degraded-mode UI is identity-only

`NodePoller::daemonStateChanged(bool)` is connected to `IdentitySection::onDaemonStateChanged` only. `NetworkSection` and `PeersSection` retain their last-seen values when the daemon goes unreachable — they don't dim, gray out, or add a "(stale)" tag. This is intentional for Phase 1: the identity section is the canonical place users look for "is my node alive" status.

**Phase 2 follow-up:** propagate degraded state to NetworkSection (dim bars + "(last seen)" annotation) and PeersSection (dim row text + "(stale)" header annotation). Trivial to add once Phase 1 ships; deferred to keep this PR scoped.
