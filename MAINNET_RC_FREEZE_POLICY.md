# Dinero Mainnet RC Freeze Policy

## Scope
This policy applies from `v1.0.0-rc1` tag creation until mainnet launch tag.

Consensus-adjacent files include (not exhaustive):
- `src/consensus/*`
- `src/daemon/block_acceptor.cpp`
- `src/daemon/daemon_app.cpp`
- `src/daemon/services/chainstate_service.cpp`
- `src/core/consensus/*`
- `include/consensus/*`
- `include/daemon/services/chainstate_service.h`

## Freeze Rules
1. No consensus-adjacent change is merged without a release-suite pass from the exact commit SHA.
2. Every consensus-adjacent PR must include:
   - a behavior statement (`refactor-only` or `behavior-change`)
   - updated/added tests proving intended behavior
   - explicit reviewer sign-off in PR notes
3. If behavior changes, PR notes must include:
   - reason for change
   - expected accept/reject impact
   - fixture or replay evidence
4. Non-consensus changes may merge independently, but must not skip CI.

## Required Gates
Run and archive outputs for:
1. `ctest --test-dir build --output-on-failure -R WalletMainnetReadiness`
2. `tests/test_acceptance_parity.sh --require-baseline` with `STRICT_INVALID_FIXTURES=1`
3. `tests/test_p2p_storm.sh`
4. `tests/test_ibd_torture.sh --profile release`
5. `tests/test_utreexo_consensus.sh`
6. `tests/test_mempool_stress.sh`

Recommended one-command runner:
- `tests/test_release_suite.sh`

## Blockers
Release candidate is blocked by any of:
- acceptance parity divergence (height, tip hash, chainwork, or fixture mismatch)
- deterministic liveness failure in P2P storm or IBD torture
- Utreexo root/commitment mismatch
- wallet readiness regression
- mempool stress crash/panic/invariant failure

## Build Discipline
1. Build from clean trees only.
2. Record binary SHA256 for release artifacts.
3. Reproducibility check required before final mainnet tag:
   - two clean builds
   - matching SHA256 for `dinerod` and `dinero-cli`

## Tagging
1. Create `v1.0.0-rc1` only after all required gates pass.
2. During freeze, any consensus-adjacent merge requires rerunning full release suite.
3. Mainnet launch tag is cut only from a commit with:
   - green release suite
   - reproducible build evidence
   - finalized release notes.
