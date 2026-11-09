# Separation gates

Gate probes for the wallet/node separation (spec section 8). All `sep_*` targets
are `EXCLUDE_FROM_ALL`; each ctest builds its target first. Run with
`ctest --no-tests=error -R '^<name>$'`.

## W6: standalone wallet (`SepWalletStandalone`)

Question: does the wallet link and run with no node code? (Spec section 8, W6
row, redefined 2026-10-31; replaces the earlier wallet-FFI/Tauri-bridge consumer,
whose link list no shipping client uses.)

Target `sep_wallet_standalone` is the W4 checker (`wallet_fixture_compat.cpp`,
same compile settings) linked only through `dinero_wallet`'s own declared CMake
dependencies: `target_link_libraries(sep_wallet_standalone PRIVATE dinero_wallet)`
(Orchard ON: plus `dinero_orchard_index_delivery`, exactly as W4). No
`dinero_core`, no `mempool_test_stubs.cpp`, no hand-listed archives; Linux adds
`-Wl,--no-undefined`. The ctest `SepWalletStandalone` builds the target, then
runs it on `fixtures/<off|on>`. Two checks make the gate:

1. the ctest (link with no unresolved symbols, then run and pass on the fixture);
2. `check_link_closure.py --target sep_wallet_standalone --gate W6` (the `W6`
   rules in `closure_rules.json` equal `W2`'s; a test pins them equal).

Orchard OFF: both must pass. Orchard ON: the verdict is the ctest alone; the
closure joins the verdict in BOTH configurations since Task 6.3
(2026-11-07): the Orchard delivery libraries no longer link node code.
`dinero_orchard_chainstate_write` and `dinero_chainstate` (spec section 8; plan
Task 6.3 removes the exception).

The ctest is labelled `separation;recorded` until stage 7 (Task 7.1).
Stage-0 result (RED, measured): see the Task 0.8a report and
`dinero-evidence/2026-10-29-separation-stage0/task-0.8a/`.
