# Durable origin for a wallet-created payment

`wallet.sendtoaddress` now uses `SignAndStageWalletPayment`. The existing signer pins the selected wallet name, session and recovery seed, resolves every selected input and signs. Before releasing that owner or calling the mempool, it commits the exact signed body, explicit recipient/amount/label, calculated fee, creation time, selected input amounts/scripts and existing send-history entry in one checked FULL SQLite transaction. A failed stage returns no signed transaction. The input rows must still exist with the exact amounts/scripts and unspent state; manual locks and retained payment reservations refuse stale selection.

The payment contract has one recipient and at most one owned change output. Every output must match that contract. The fee is checked from exact input and output sums and must agree with the signer result. The supported recipient encoding is a valid Dinero witness address. Explicit unconfirmed-chain input selection cannot be staged unless the wallet's own durable unspent row is present; merely observing a mempool output is insufficient.

## Retained owner

The `pending_payment_owner` BLOB in `wallet_meta` holds the complete retained set in one AES-GCM envelope under a domain-separated seed-derived key. Its canonical `DNPP01` payload binds the persistent wallet identity, every signed body, intent and selected input. Reservations are derived from these records, rather than maintained in a separate unauthenticated index. Parsing validates exact framing, terminal reads, canonical transaction bytes and txids, input correspondence, unique payment IDs/outpoints, intent and fee sums. Required send-history metadata must agree; chain confirmation fields remain independently mutable observations.

The first stage adds and populates the column in its transaction. An installed column with NULL, malformed, unavailable or unauthentic contents refuses. A wallet without this column has no installed tracking owner; that is **not** evidence that its earlier pending payments have been recovered. Schema removal and a coherent whole-database rollback remain outside this owner’s completeness guarantee. Capacity is bounded to 4096 retained payments, 4096 inputs per payment and 16 MiB of encoded owner data. Limits refuse rather than drop records; this is not load qualification.

`wallet.listpendingpayments` authenticates and returns the retained signed bodies and original intent for the selected unlocked wallet. Its `retained` state makes no claim of admission or confirmation. `isUTXOLocked`, lock listing and locked-balance queries include authenticated reservations. Manual unlock/unlock-all cannot delete them, and the old process-local abandonment API refuses these payments. Reads of an installed owner require an available unlocked seed. Unlock and encryption transitions authenticate the owner before publishing changes, and seed replacement cannot orphan it.

## Submission and remaining lifecycle

The wallet lease ends before admission/relay. Rejection, missing service, exception, or an already-known result retains the original body and reservations. The RPC exposes its retained txid once staging succeeds. There is no blanket release on non-OK admission. Existing chain confirmation updates do not replace the retained origin.

This change covers the actual new `sendtoaddress` stage and shared reservation readers. Generic raw signing preserves its existing contract. `sendmany` replacement lineage, wallet-funded consolidation, explicit safe abandonment, selected-source confirmation/conflict/disconnect reconciliation, readmission, cross-account/Orchard reservations and all notification consumers still require integration. There is no automatic rebroadcast or claim of complete historical pending recovery, complete wallet inventory, readiness or release qualification.

## Verification scope

The new `WalletPendingPayment` CTest uses real wallet creation/encryption, historical and current keys, actual signing and SQLite. Cases cover retained body/history/reservations/reopen and listing adapter, stale coins and locks, transactional history failure/COMMIT refusal, borrowed transactions, corrupt/unavailable owners, interrupted reads and intent mismatch. Local ON/OFF builds and sanitizer qualification are tracked in private evidence; a registered test is not an executed pass. The fixture does not execute the `sendtoaddress` transport, actual mempool admission, network relay, crashes or power loss. Mainnet activation remains unset.
