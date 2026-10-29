# Storage test archive linkage

The full Linux default and QUIC builds, and Core Heavy on `5ecf72c`, failed while
linking `test_pruning` and `test_header_status_bits`. Transitive wallet members
introduced references to UTXOIndex and `open_sqlite` after GNU ld had scanned the
archives that supply them. The wallet's daemon-only Mempool reference was also
unresolved. These failed builds do not establish test execution.

Both storage targets now use the existing `mempool_test_stubs.cpp` isolation seam.
Neither fixture constructs a mempool or tests its behavior. Their real ChainDB,
header metadata, pruning and serialization fixtures remain unchanged. No new
stub, production implementation change, weakened assertion or deadline is added.

On Linux the same fourteen mutually dependent project archives are grouped as
in the existing wallet input/maturity targets, using raw linker group flags
compatible with CMake 3.20. Other platforms retain their original library lists.
The independent Orchard lane builds both actual targets, requires their enabled
registrations, executes them and verifies the five pruning case markers and the
12-property header-status completion marker. Inventories and logs are retained.
Root Orchard selectors are unchanged; actual completed executions determine
qualification counts.

Fresh genuine Orchard ON/OFF configurations built the complete daemon and both
declared storage targets. Both CTests passed in each configuration, with all five
pruning markers and the header's 12-property completion verified. Fixture and
existing stub bytes match the parent; the actual compile commands retain enabled
assertions. The OFF service/wallet graph was checked. No new sanitizer or copied
omission-control claim is made for this CMake/workflow-only repair.

Mac qualification does not reproduce or prove repair of GNU archive order.
New Linux execution is required. This build repair does not qualify wallet
unlock, pending ownership, production mempool behavior, startup, crash/powerloss,
activation or release. Mainnet activation remains unset.
