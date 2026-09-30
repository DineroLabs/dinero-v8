# Wallet-to-vault observation ownership

The actual wallet worker captures the current vault service, operator script/account and chain-service lifetime before acquiring its wallet database lease. After the existing index-first/wallet-second commits it updates the original wallet height while pinned, releases the lease, then invokes that captured observer. There is no later WalletManager access. Captured work retains its original service and binding across runtime detach or operator changes.

The observer checks the complete display-order block hash and takes any selected-tip read under the real chain-service lifetime/activation owner before service mutation. It releases that chain owner before deposit mutation and the existing whole-tip service reader. No source fallback is taken after a configured source fails. Existing no-source injected components retain deposit-height behavior. This corrects a source-observed lock-order violation; it does not establish a cause for any CI timeout or shutdown failure.

Four patched-path cases exercise the actual worker with real wallet/index SQLite and runtime: committed rows/height and zero active wallet leases at the callback; retained original service/account across detach; hash refusal/unmatched scripts without effects; and failed wallet write followed by successful retry. Component callbacks use synthetic chain observations. No unsafe-original execution, deadlock/race reproduction, timing control, production change or physical-power-loss claim.

The legacy notifications remain best-effort. A successful wallet commit is not a durable vault acknowledgement. Missing notification replay, production durable state/withdrawal-owner attachment, loss-accounting invariants, all-consumer provider and release qualification remain open. Locally qualified in the combined durable-vault batch.

## Combined qualification scope

This change was locally qualified together with wallet observation, durable state, retained withdrawals, dispatch lifetime, payment binding, runtime restore refusal, and reservation metrics. The earlier per-component pipeline was superseded before source application. Locally qualified in the combined durable-vault batch. The combined lane retains every case and deadline: fresh full ON/OFF declared targets,119/114 component CTests plus10daemon and2combined each, and separate full linked-project replay155 and wallet-RPC50 ASan/UBSan case groups. No per-component intermediate-source qualification is implied. Production durable runtime attachment and remaining release gates stay open.
