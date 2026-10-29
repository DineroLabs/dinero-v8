# Wallet-bound retained vault withdrawals

Locally qualified in the combined durable-vault batch.

The durable vault owner can prepare a new withdrawal with explicit integer fee rate, maximum fee and audit context. These terms commit with the request. DNVS02 encodes them; DNVS01 remains readable with absent terms. Historical requests receive no inferred terms and cannot enter signing until a separately authorized preservation policy exists.

The service checks collective Pending/Signing amount reservations against ledger spendable funds. Retained and legacy Broadcast entries already lock ledger funds and are not double-counted. It commits Pending -> Signing before external work, releases the vault mutex/SQLite/recovery-key owners, and then calls the concrete wallet dispatcher. Only that call's newly committed transition can start dispatch. Reopened or concurrent Signing retries can resolve an existing wallet body only. Missing or unavailable bodies leave requests reserved. This is a deliberate recovery boundary; completing recovery of a crash before initial dispatch remains open.

The dispatcher authenticates the existing vault identity, selected wallet session, network/genesis and exact Signing request. It invokes the actual strict integer `wallet.sendmany` path through an internal exact service/name/session entry point. The public JSON API gains no wallet-session override. The handler retains WalletUse for its synchronous lifetime and keeps all existing selected-session/key-owner checks.

After dispatch, including rejection, error or unknown submission outcome, the bridge reads the authenticated wallet payment owner by the exact request intent. It validates canonical signed bytes, transaction identity, fee limit and the unique exact recipient output. The vault records the actual output index, body SHA256 and fee in WithdrawalPaymentRetained and commits the matching ledger entry before publication. Every saved retained reference is reauthenticated against the wallet payment envelope and history in the same state-store transaction when reading or staging vault state. No result is labeled broadcast or confirmed from an RPC response. Retrying an already retained request does not submit another body.

The new optional dispatcher factory runs during service preparation before initial checked commit/publication. Existing factories without a dispatcher remain fail-closed for durable signing. Runtime attachment and explicit creation/reopen RPC integration remain open. The explicit WalletWithdrawalDispatchOwner gates the complete synchronous call and must close before ExecutionContext or service destruction. Its production startup/shutdown installation remains open.

## Qualification scope

Eight mandatory component cases use the actual wallet, state store, service, handler and signing path with synthetic funded coins and explicit ingress: accepted retention/reopen; rejected retention; wallet commit followed by vault write refusal and reopen resolution; missing body preserved without generation; exact terms and collective reservations; exact wallet session/service refusal; genuine recipient output index 1; legacy DNVS01 and absent terms. Full ON/OFF declared targets and prior tests remain required. Sanitizer evidence covers every linked project C++ unit in the selected wallet RPC binary; separate daemon/replay binaries, external/Rust/C/PQClean code and platform qualification remain distinct. No unsafe-original or synchronization-removal control is used.

## Remaining release work

Persistent caller-supplied withdrawal inclusion refuses. Canonical payment inclusion, confirmation, reorg/readmission/relay/mining, operator-fee reservation policy, cancellation/lineage, catalog/deletion/backup rollback completeness, runtime provider/consumer installation and whole-node lifecycle remain open. An authenticated present-state snapshot or retained payment is not readiness. Mainnet activation remains unset.

## Combined qualification scope

This change was locally qualified together with wallet observation, durable state, retained withdrawals, dispatch lifetime, payment binding, runtime restore refusal, and reservation metrics. The earlier per-component pipeline was superseded before source application. Locally qualified in the combined durable-vault batch. The combined lane retains every case and deadline: fresh full ON/OFF declared targets,119/114 component CTests plus10daemon and2combined each, and separate full linked-project replay155 and wallet-RPC50 ASan/UBSan case groups. No per-component intermediate-source qualification is implied. Production durable runtime attachment and remaining release gates stay open.
