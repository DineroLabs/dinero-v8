# Rust dependency security checks

The standalone workflow audits both committed node dependency lockfiles: the
Orchard backend and the legacy Bulletproofs FFI build. It runs for relevant
pull requests and pushes, daily on the default branch, and by manual dispatch.
The schedule takes effect only after the workflow reaches the default branch.

Each scan uses pinned cargo-audit 0.22.2 with fresh RustSec advisories. Known
vulnerabilities, unsoundness notices, yanked crates, missing lockfiles and
scanner/network errors fail. Maintenance notices remain visible. There are no
ignored advisories, automatic dependency upgrades or platform filters. Raw
reports, exit codes, lockfile hashes, source identity and advisory database
commits are retained even on failure. Both matrix entries run if one fails.

The existing Orchard component audit remains required. This independent job
adds daily monitoring and the second node lockfile without running project
regression tests. It does not cover unlocked desktop/mobile Tauri manifests,
prove dependency reachability, or replace integration and cryptographic review.
Any additional shipped Rust component must supply a committed lockfile and be
added to this audit inventory before release.
