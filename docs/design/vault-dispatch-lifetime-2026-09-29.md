# Vault wallet dispatch lifetime

Locally qualified in the combined durable-vault batch. Not installed in production runtime.

An application-owned WalletWithdrawalDispatchOwner now controls every concrete vault callback carrying an ExecutionContext. Factories and services may retain its closed state, but cannot acquire another call after Close or owner destruction. The caller closes before stopping or destroying the daemon context and holds no wallet, chain or SQLite owner while doing so.

Each synchronous Resolve or DispatchNew retains a thread-bound call owner through authentication, wallet work, external submission and final retained-body read. The gate mutex is released before those operations. Close first rejects attempts from an active callback on its own thread, then closes new admission and waits for active calls. Destruction closes; destruction on the active callback's thread terminates as a lifetime contract violation. Failure paths release the call owner. The source implements cross-thread drain; the new deterministic component cases cover quiescent close/destruction, retained factory/service refusal, same-thread explicit-close refusal during actual submission, and release following a failed dispatch. They do not claim concurrent shutdown/race/deadlock qualification.

The old bare-context factory is replaced; eight prior retained-payment fixtures acquire an explicit application owner without changing their assertions. Four new cases use the actual service/wallet/state/RPC paths. Runtime attachment and daemon shutdown wiring are still required before this owner can protect production calls. Closing an owner is not a payment cancellation, reservation release or durable completion acknowledgment.

## Combined qualification scope

This change was locally qualified together with wallet observation, durable state, retained withdrawals, dispatch lifetime, payment binding, runtime restore refusal, and reservation metrics. The earlier per-component pipeline was superseded before source application. Locally qualified in the combined durable-vault batch. The combined lane retains every case and deadline: fresh full ON/OFF declared targets,119/114 component CTests plus10daemon and2combined each, and separate full linked-project replay155 and wallet-RPC50 ASan/UBSan case groups. No per-component intermediate-source qualification is implied. Production durable runtime attachment and remaining release gates stay open.
