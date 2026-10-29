# Full-node mining coin provider

Actual mining.getjob and the shared daemon assembler now use the selected consensus coin set for full-node mining, matching getblocktemplate. Inputs owned by another wallet are chain inputs and must not depend on the local wallet index. Covered template operations retain the existing selected-chain guard and pool owner through construction.

The explicit stateless configuration retains its existing wallet adapter. Frozen/pre-base coin membership and oracle resolution remain separate work; this change does not certify CSN or AssumeUTXO mining, independent historical parent provenance, all shutdown subview lifetimes or release readiness. Mainnet remains unset.

The new isolated two-full-node fixture gives only the source node the funded wallet, imports its actual accepted blocks into an independently created miner, verifies the miner has no source wallet coins, admits a genuinely signed transaction, solves mining.getjob and submits mining.submit. It checks the selected accepted block contains the foreign transaction, restarts the miner and repeats. Existing tests remain unchanged.

Fresh local builds of the full daemon with the Orchard backend enabled and disabled each pass 36 selected component tests and six daemon integration tests. All 408 linked project C++ translation units in the enabled daemon were freshly built with ASan/UBSan; both the new two-node fixture and the existing mining RPC result fixture pass against that daemon, with 1,631 source/header input hashes unchanged. External libraries, Rust, C, PQClean, Objective-C++ and the disabled-backend binary are outside this sanitizer scope; macOS leak detection is off.

The first local compile exposed an incorrect namespace on GetConfig, corrected to the existing global function. The first integration run encountered the server's explicit pre-dispatch rate limit while copying blocks. The fixture retries only that exact refusal, with a bounded retry count; other errors propagate, and server limits, assertions and the test deadline are unchanged. Final results above follow both corrections. No original-source regression or omission control is claimed.

Both mining RPC tests have the integration label, which excludes them from the full workflow's broad sweep. The full workflow now executes each by exact name, with a missing registration treated as an error, and retains its log. The separate Orchard lane also requires actual execution and completion markers. Local lane mapping establishes the repaired selection; current-source Linux execution remains required.
