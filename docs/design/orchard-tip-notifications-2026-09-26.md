# Miner notification at active-tip publication

The shared active-tip setter now signals miner longpoll after the pointer and
published value agree. It compares validity, hash and height; pointer replacement
or re-publication of the same identity does not advance the generation. Rollback,
same-height replacement, initial publication and clearing a tip do. This is a
process-local signal, not an event-delivery acknowledgment or consensus rule.

The observer mutex is released before signaling; the selected writer lock still
serializes the transition. Signaling, including singleton initialization, is
inside a noexcept boundary: synchronization failure after a durable write cannot
return as an ordinary prewrite refusal. Fallible diagnostics follow the signal.
The legacy connected-block callback no longer sends a duplicate signal. The
actual typed connect/disconnect routes already use the shared setter, so they
receive this built-in consumer without a synthetic historical Block or a new
permissive notification provider.

The getblocktemplate handler uses waitIfCurrentTip, capturing the generation
before reading the current database tip. Capturing it after that read can miss a
transition between the two observations and unnecessarily wait for the timeout.
A mismatching tip skips the wait. Template construction reads fresh state after
waiting as before. No proof, transaction-admission or miner-readiness gate changes.

## Local regression scope

The existing real-service tests check generation changes under allocation refusal,
rollback, null publication, same-height replacement and unchanged re-publication.
Generated-store connect/disconnect tests require the wake generation to have
advanced exactly once before the prepared consumer observes committed state.
A deterministic test publishes a real service tip within the wait predicate;
it verifies that the captured earlier generation observes that change, without
sleeping or depending on thread scheduling. It also covers a mismatched tip and
an unchanged matching tip.

These checks exercise the real shared setter and typed single-block service paths.
They do not qualify a running RPC server/miner under load, all reorg orchestration,
or durable wallet delivery. Longpoll has no durable cursor: a restarted process
has no old HTTP waiters. Production recovery for wallet, mempool readmission,
proof caches, relay and configured oracles remains required. Mainnet activation
stays unset and the complete typed notification provider remains uninstalled.
