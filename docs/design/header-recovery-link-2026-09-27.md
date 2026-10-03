# Header metadata recovery test linkage

The full, QUIC and core-heavy Linux jobs at `3e07aeb7` all stopped while
linking `test_header_metadata_recovery`: the transitive wallet object references
`Mempool::isOutputSpentInMempool`, whose production implementation belongs to
the daemon executable rather than a linked library. The narrower Orchard
component run did not build this target and therefore did not qualify its link.

The test target now includes the existing `mempool_test_stubs.cpp` isolation
source, as other non-mempool fixtures do. This fixture operates on actual
ChainDB, archival block storage and metadata recovery; genesis setup passes a
null UTXO index. It does not exercise mempool semantics. Production source,
fixture assertions and deadlines are unchanged. This repair does not qualify
pending transactions, mempool admission or wallet selection.

The independent Orchard workflow now builds this target, requires its enabled
CTest registration, executes it verbosely and checks the existing completion
marker. Its inventory and execution log are retained. The original 46-test
Orchard selectors remain unchanged. The added independent lane is one existing
CTest; future totals require actual execution verification.

Local qualification built full daemons and the declared recovery target in
fresh backend-enabled and backend-disabled CMake configurations. The existing
recovery CTest and its completion marker passed in each configuration, including
repeat execution after copied link probes. Omitting the isolation object from
both copied Mac link commands still linked successfully: those probes do not
reproduce the Linux failure and are not passing negative controls. The actual
three Linux link failures are retained separately. Exact Linux full/default,
QUIC and core-heavy qualification still requires fresh CI at the repaired
source. No unchanged-head retries are used. No new sanitizer qualification is
claimed for this build-only change; existing production sanitizer receipts
retain their original scope.

Discovery, explicit key initialization versus recovery, staged unlock,
pending ownership, all consumers and release gates remain open. Mainnet
activation is unset.
