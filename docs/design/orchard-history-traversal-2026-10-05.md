# Complete historical transaction traversal

The activation-parent preparation already captures every accepted historical body and a process-local authenticated transaction-membership tree. Querying a few candidate IDs did not enumerate that complete set for the future compact-node catalog.

`RuntimeReplayDiskMembership::ForEach` now walks reachable nodes from the owned root, using a fixed array of at most 257 pending frames. It verifies each record through the existing authenticated spool reader (including the terminal SQLite result), checks increasing branch bits, the leaf's path bits, strict key order and the exact expected count. Missing records and read failures throw. The traversal does not enumerate raw SQL rows, so deleted referenced nodes cannot become successful end-of-input.

`OrchardHistoryCapture::RecordBody` maintains the successful unique transaction count alongside its existing root and body digest. It publishes that count only after the same spool batch commits. `Finish` checks the entire reachable membership inventory before freezing the capture or marking it finished. `ForEachTransaction` then exports actual transaction IDs from a finished capture, never its disposable root or SQLite handle.

Visitors run after each spool read has released its statement and mutex. A visitor can receive a prefix before a later read or count check fails. Callers must stage privately and discard that preparation on any exception or false visitor result. Only a successfully returned exact count permits the next preparation step. This API performs no durable catalog publication.

The actual service still owns independent genesis-to-parent consensus replay, records bodies only after successful replay, and requires both results to finish. Capture completion alone is not proof of scripts, proof of work, fees, monetary accounting or snapshot provenance. The durable catalog must consume the same completed replay and capture, bind the exact selected parent, and publish atomically with the canonical transition.

## Regression coverage

Four cases extend the existing history-capture suite without replacing its prior eight cases:

- Exact transaction inventory compared with every body accepted by the real parent replay.
- Late SQL interruption, denied reads, visitor refusal/exception and successful retry.
- Missing membership leaves, count mismatches, and commit refusal without count publication.
- The maximum 256-branch path with 257 distinct keys, ordered traversal, persistent old roots, duplicate insertion and wrong expected counts.

Fresh genuine Orchard ON and OFF builds of `dinerod` and the declared replay target passed. The selected ON run executed five CTests containing 25 GTest cases (12 history-capture cases and 13 prior replay cases). The OFF run executed one CTest containing ten cases. These are six executed CTests and 35 internal cases, not the entire registered test suite.

The linked ON replay test graph was rebuilt with ASan/UBSan: 319 project C++ files plus the bundled Bech32 C++ file. All 25 selected ON cases passed. Three copied header controls separately rebuilt all 320 linked C++ files and removed only the final count check, visitor-refusal check, or completion traversal. Each failed its intended named assertion without a fixture exception or sanitizer diagnostic; the unchanged implementation then passed all four new cases. Compiler dependencies verify that the fixture and real chainstate caller consumed the copied header, and the link maps contain no project C++ archive members.

Other external libraries, Rust, C and PQClean were outside this sanitizer graph; macOS leak detection was disabled. The full daemon and OFF binary were normally built and tested within the scope above, not covered by this instrumented replay binary. No initial original-source failing-test claim: the new tests and implementation were introduced together.

## Remaining work

This is a catalog-input prerequisite. It does not persist legacy creation metadata or complete transaction membership across process restart, implement compact canonical connect/disconnect, enable CSN ingress, or qualify CSN shield/send/unshield, reorg or reindex. It adds no activation height and makes no release-readiness claim.
