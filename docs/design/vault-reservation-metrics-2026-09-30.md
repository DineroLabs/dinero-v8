# Vault balance queries include durable withdrawal reservations

Durable admission already reserves Pending and Signing requests before an authoritative wallet payment body exists. The live balance queries previously reported those funds as spendable until the later ledger lock. The service now uses the same checked reservation sum for admission and account metrics, subtracts it from spendable and adds it to locked. A retained payment already has a ledger lock and is counted once. All fields are captured under the existing service mutex, with no SQL/chain/signing callback or mutation.

Legacy in-memory service metrics retain their existing behavior. These are live captured balances, not durable freshness, confirmed payment, chain readiness or an authenticated completeness claim. Fees, settlement allocation, cancellation and reorg accounting remain separate. No historical ledger rows, fees, labels or owners are rewritten. Missing signing bodies remain reserved and do not authorize regeneration.

Four patched actual wallet-state/withdrawal cases cover collective Pending reservations and reopen, unresolved Signing, the transfer into a retained ledger lock without double counting, and SQL write/commit refusal plus read-only metrics. Prior fixtures/assertions/deadlines remain unchanged. Locally qualified in the combined durable-vault batch.

## Combined qualification scope

This change was locally qualified together with wallet observation, durable state, retained withdrawals, dispatch lifetime, payment binding, runtime restore refusal, and reservation metrics. The earlier per-component pipeline was superseded before source application. Locally qualified in the combined durable-vault batch. The combined lane retains every case and deadline: fresh full ON/OFF declared targets,119/114 component CTests plus10daemon and2combined each, and separate full linked-project replay155 and wallet-RPC50 ASan/UBSan case groups. No per-component intermediate-source qualification is implied. Production durable runtime attachment and remaining release gates stay open.
