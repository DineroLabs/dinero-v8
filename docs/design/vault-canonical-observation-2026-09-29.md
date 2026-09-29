# Checked canonical vault output observations

Locally qualified.

The vault chain closures retain the configured chainstate service and acquire its real index lifetime owner before a selected-chain read. Canonical hash capture checks live, durable, validated and coin tips plus the height mapping. Inclusion reads the exact selected canonical body and preserves historical and Orchard families, including the historical coinbase in a mixed block. It reports inclusion only for the exact transaction and an existing transparent output index. A complete read can report absence; unavailable source/body state or a changed expected hash throws instead of returning an inclusion claim.

The reorg watcher stores the same hash used by its completed inclusion check, without a second independent hash lookup. A refused check preserves its prior deposit observation for retry. The existing boolean callback interface remains; operational errors propagate to its caller. This does not add deposit credit, mutate chainstate, certify that an output is unspent, or authenticate a whole historical ledger.

Four new patched-path cases are planned (two also backend-OFF). They cover missing sources, actual deposit-flow observation preservation and retry, real historical and mixed canonical transaction/output reads, real typed disconnect/reconnect, captured source lifetime, and unavailable-store retry. Code and fixtures were prepared together. No unsafe-original, race, deadlock, synchronization-removal or initial-red execution is claimed.

The actual vault notification provider, global runtime synchronization, atomic ledger persistence, all-deposit completion, withdrawal lifecycle, other consumers and release readiness remain open. No production runtime is enabled and mainnet activation remains unset.

## CI inventory diagnostics

Retain the generated real mempool fixture target list with root artifacts. The exact twenty-one-target guard now reports the observed count and names before refusing a mismatch. Test registration, execution and success checks remain unchanged.

## Daemon shutdown ownership

The runtime canonical-read closures retain chainstate. The unchanged actual daemon lifecycle fixture detected that shutdown had no corresponding vault-runtime release. `DaemonApp::Stop` now calls `ShutdownVaultRuntime` after the registered services stop and before clearing the daemon context. Runtime publication, callbacks and operator state are cleared before flushing the detached store, so a flush exception cannot retain the old runtime. The daemon reports a flush exception and continues teardown. This does not establish vault ledger replay, durable credit/withdrawal completion or runtime shutdown draining. Actual RPC wiring currently defaults `vault` to true; this change preserves that policy.
