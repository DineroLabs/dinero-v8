# Selected-wallet transaction signing

The sendtoaddress, sendmany and consolidate handlers capture the requested wallet name and session before selection, then invoke one wallet-owned signing entry point after transaction construction. The entry point checks every input against its selected outpoint before reading keys, reacquires the actual database lease, requires the captured session, and pins the existing recovery seed through all signatures. Historical imported scalars retain their exact historical tweak policy; modern imports and HD keys retain canonical policy. No imported path is fabricated and no alternate key or HD-label fallback is introduced.

PQ signing uses the existing wallet master and opens only an existing store. Caller and provider master copies are cleansed. Signing releases the key pin, store and lease before the RPC performs admission or broadcast. Failure returns no signed transaction, including failures in the lower-level signer's final local validation. Sendmany uses the existing builder's preview construction followed by the same owned signer; it no longer excludes an otherwise valid historical key merely because it has no HD path.

## Qualification scope

The new WalletOwnedSigning cases invoke the actual shared production entry point, with encrypted historical, modern, HD and PQ fixture owners. They check ordinary consensus signatures and PQ verification, changed-message rejection, absent-store refusal without creation, selection/pin/borrowed-transaction behavior, complete outpoint checks before reads, and refusal without fallback or partial output. The original four HistoricalRpc cases remain separately registered. These are component fixtures with synthetic coins, not execution of the three complete send RPC flows or mempool admission/broadcast.

Fresh ON/OFF daemon and declared wallet builds, instrumented linked-source execution and copied omission controls are required before qualification. Actual results are retained privately; this document does not assert pending CI outcomes.

## Remaining ownership boundaries

The captured identity is revalidated for signing. It does not make preceding coin selection, change-address issuance or following history/broadcast one transaction. A selected-wallet change during earlier RPC work can still cause earlier unused address issuance before signing refuses. Per-key reads and the separate PQ store are not an authenticated complete catalog or a multi-process snapshot. Whole RPC authorization, pending reservations, PSBT and new Orchard shield/send/unshield ownership remain separate gates. Mainnet activation stays unset; no readiness or release claim follows from these tests.

## Local result

Fresh backend ON and OFF builds completed the full daemon and four declared wallet targets; each configuration passed all 14 selected CTests. The actual shared-entry-point fixture linked 266 project C++ translation units, all freshly compiled with ASan/UBSan after the final C++ edit. Seven cases passed with 1,486 stable source/header inputs. Three copied-source omissions each failed its intended assertion without a fixture exception or sanitizer diagnosis; restoring the owner passed all three new cases. Original raw-RPC case bodies and all 94 prior workflow CTest commands remain unchanged. Production and new fixtures were introduced together, so no original-source failing-test claim is made.

External libraries, Rust, C and PQClean were not instrumented; macOS leak detection was disabled. The daemon, OFF binary and separate wallet test binaries are outside the 266-file instrumentation graph. The full ARM RocksDB gate remains open. Linux qualification is recorded separately when actual CI completes.
