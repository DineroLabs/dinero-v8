# Orchard compact storage compatibility binding

The canonical compact writer now stages an explicit network/profile storage binding in the same private batch as first enrollment. The binding contains network, genesis, Orchard branch/activation and Utreexo leaf-encoding activation. Existing compact transitions require the exact binding; all writers recheck the captured value before committing. Abandoned preparation does not install it. Boundary undo retains it.

The full canonical writer, current full-state selected audit, full-state service startup and full reindex owner refuse a marked database. Unknown or malformed bindings refuse too. The service checks before creating auxiliary indexes or loading legacy shielded state. This prevents a config change or a boundary undo from silently treating compact storage as a full coin/forest store.

This record is a compatibility guard, not a historical authentication certificate. Checksums and encoding do not confer authority. Its absence cannot enroll a compact owner: that still requires the existing private completed parent replay. Missing binding under a live compact owner refuses. No conversion, coin deletion, historical replay, catalog reconstruction or new-owner startup is performed by the binding.

## Qualification scope

Two new component cases cover atomic first installation/abandonment, full-writer and full-reindex refusal, retained binding through undo/reconnect, and missing/malformed/changed records refusing before publication. Existing fixtures and deadlines remain unchanged. The production startup guard is compiled; these component cases do not execute full service startup. Implementation and fixtures were introduced together, so there is no initial-original-RED claim.

Fresh full daemon and replay-target builds passed with the Orchard backend enabled and disabled. The 17 selected CTest executions passed: 67 enabled-backend cases and 15 disabled-backend cases. Fresh ASan/UBSan instrumentation of all 323 linked C++ files (322 project files and bundled Bech32) passed the same 67 enabled-backend cases. Other external libraries, Rust and C/PQClean remain outside that instrumentation; macOS leak detection is disabled. This does not qualify the full daemon or disabled-backend binary under sanitizers.

Two copied data-only controls each rebuilt the entire linked graph and failed their intended assertions: omitted storage-binding persistence and omitted profile comparison. Neither produced a fixture exception or sanitizer diagnosis. All 30 selected cases passed again against the unchanged implementation. No synchronization, commit ordering, assertions or deadlines were changed. Exact source and results are preserved privately; these are local component qualifications, not Linux CI or release qualification.

## Remaining integration

Authenticated fresh compact startup and the reindex promotion handoff remain required, including explicit mode selection, exact selected domain/tip/catalog/body/undo/journal/retirement checks, and below-activation replay behavior. The guard must remain until those actual callers and configured consumers are integrated and qualified. The binding is not rollback/deletion completeness, multi-process protection, readiness or release qualification. Production CSN refusal and unset mainnet activation remain.
