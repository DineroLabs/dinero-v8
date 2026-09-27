# Initial mining and coinbase fixture linkage

The full default, QUIC and Core Heavy Linux builds at `cf4657057` passed the
previously repaired storage links and then failed linking `test_mining_height2`
and `test_wallet_height2_coinbase`. Both reported the unresolved daemon-only
`Mempool::isOutputSpentInMempool` dependency from the transitive wallet object.
These failed builds provide no CTest execution qualification.

Both targets now include the existing `tests/stubs/mempool_test_stubs.cpp` seam.
Their real ChainDB, BlockStorage, UTXOIndex, genesis, archival and maturity
fixtures remain unchanged. Neither fixture constructs a mempool or exercises
mempool behavior. No new stub, production implementation, test assertion or
timeout is introduced or changed. The historical target names contain height 2;
the existing fixtures actually exercise height 1 and retain that behavior.

The independent Orchard lane now builds and requires both enabled registrations,
executes them, checks each distinct completion marker and retains the inventory
and verbose log. Existing root selectors remain unchanged. This extends the
scope of that component lane; it does not make it equivalent to the full suite.

The three full Ninja build steps now pass `-k 0` to collect all independent build
failures before stopping. Ninja still returns nonzero when any target fails, so
subsequent test gates do not run on a failed build. A separate small CMake/Ninja
check executed two intentionally failing targets and independent work, then
confirmed nonzero exit. No production tests or build failure gates are bypassed.

Fresh genuine Orchard ON/OFF configurations built the full daemon and both
declared targets. Both actual CTests and their distinct completion markers
passed in each configuration. Fixture/stub bytes match the parent, assertions
remain enabled, and the OFF service/wallet graph was checked. This build-only
change makes no new sanitizer or original-source Mac failure claim.

Mac link success does not reproduce or qualify GNU linker behavior; new Linux
execution is required. These isolated fixtures do not establish full-node
activation, production mining, pending ownership, shield/send/unshield,
crash/powerloss, reorg/supply or release readiness. Mainnet activation is unset.
