# Orchard dependency provenance

Based on Orchard 0.16.0 from crates.io, upstream commit `8190e4d9b7e9c0140964e88b71f95276d2b50470`.
Original crate archive SHA-256: `4ae7ceb7b5387bd76cf3c0712f57708795c925cecf097036cedb60b5c3706e8f`.
Upstream: https://github.com/zcash/orchard/tree/8190e4d9b7e9c0140964e88b71f95276d2b50470

The upstream MIT and Apache-2.0 licenses are retained. The registry cache marker `.cargo-ok` is omitted.

Local changes to upstream package files:

- `Cargo.toml`: a pinned rand 0.8.8 dependency for the saved profile-1 shuffle.
- `src/builder.rs`: explicit `build_with_profile1_shuffle` entry point and a direct RNG adapter for the original shuffle. The default builder, free bundle function and PCZT path retain the current shuffle. Both paths use the same validation, circuit and proof implementation.

`Cargo.toml.orig` and `.cargo_vcs_info.json` describe the original upstream package. This is a locally patched dependency, not an unmodified upstream distribution. The root crate enables Orchard's zeroize feature. Preserve and requalify the local patch when upgrading the dependency.
