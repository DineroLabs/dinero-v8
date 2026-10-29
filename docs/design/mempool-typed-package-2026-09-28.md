# Typed mempool dependency policy

Actual canonical historical admission now calls one pool-locked package evaluator over immutable transaction bodies. Every ancestor and descendant traversal reads the body’s typed input outpoints. Shared ancestors remain deduplicated and pending replacement removals remain excluded. The complete ancestor inventory is published to the caller only after all checks pass, and actual admitted-entry fee/size/VWU aggregation consumes that same inventory. Rejection counting remains in the real admission caller; test-only preflight preserves pool state.

Existing count and byte limits are unchanged: 25 ancestors/descendants, ordinary 101 KiB and the existing active historical shielded-resource 600000-byte profile. An Orchard body does not itself select the larger historical shielded-resource profile. The existing activated historical-resource-member rule still governs a package containing such a historical member; no new Orchard-specific byte profile is introduced. This policy evaluator does not validate proofs, authorize transactions, resolve selected-chain provenance, or enable Orchard admission. Existing Orchard ingress remains unavailable.

## Qualification

Fresh backend ON/OFF full daemon and 46 declared component targets passed. Each configuration passed 50 component CTests and seven daemon CTests, including the repaired pre-base fixture. All 76 linked project C++ files were freshly instrumented with ASan/UBSan; 1306 source/header hashes stayed stable and all four enabled cases passed. External/Rust/C/PQClean code, the OFF binary, daemon wiring and network transport are outside this sanitizer result; macOS leak detection was off. Local QUIC was disabled. Compiler pools were sequential with at most four jobs. Verification confirmed 1220 other prior fixture files and 184 prior CTest commands unchanged. New cases separate real signed historical admission/replacement from isolated structural dependency-policy graphs. Existing package-bound and replacement fixtures remain unchanged. No unsafe-original or synchronization-removal controls.

Selected-parent validation, Orchard nullifier ownership and admission, typed privacy/economic metrics, selection/mining, connected-block and reorg consumers, production notifications, and release readiness remain open. Mainnet activation remains unset.

## Pre-base fixture classification repair

The completed broad-test portion of Linux run 36453963656 reported 608 passes and one failure in AssumeUtxoPrebaseMempoolResolution. Its actual node response refused the spent input with validation-unavailable. The old fixture treated every response except the former not-found wording as admission. The unchanged fixture reproduced that reporting failure against the already-qualified 32f8a7b daemon locally.

The fixture now requires structured RPC refusals at the specific script stage for the live unsigned input and live pre-base authorization stage for the spent input, then checks both decoded transaction identities are absent from the actual mempool. Production pre-base guards are unchanged. The existing deadline and setup remain. No unsafe-original/neutered mode was executed. The updated fixture passed against the same qualified 32f8a7b daemon and then passed its actual CTest on both fresh final ON/OFF builds. The original 900-second deadline was retained.
