# Shield input selection across wallet notification delays

## Failure and behavior

Block connection updates consensus state and removes confirmed transactions from
mempool before the asynchronous WalletWorker updates the wallet and its UTXO
index. A second `wallet.shield` during that interval could select the first
shield's spent input, generate a proof, and fail admission with `Input UTXO not
found`. The existing admission checks rejected the invalid spend and rolled back
wallet persistence; this change fixes selection before proof generation.

Both the signed build and the automatic-fee input-count probe now share one
candidate filter. Wallet ownership, derivation paths, minimum confirmations and
wallet locks still come from the wallet. Candidate coins must also exist in the
current confirmed chainstate, remain unspent in mempool, satisfy next-block
coinbase maturity, and match the wallet's amount, script, height and coinbase
metadata. Confidential outputs are not transparent shield inputs.

## Utreexo and concurrency contract

`Mempool::getConfirmedWalletCoins` returns copied entries in candidate order. It
acquires the existing chainstate ingress guard before the mempool read lock,
matching admission's lock order. A configured but unavailable guard fails closed.
No wallet callback, signing or proof generation runs while these locks are held.

The lookup uses the same sources as admission/template selection:

- Stateful nodes use the active consensus map, including imported snapshot coins
  that intentionally have no ordinary ChainDB UTXO row.
- Stateless nodes retain their configured ChainDB fallback for post-base coins.
- Frozen pre-base coins require the existing live-forest resolver. A stale
  auxiliary row alone cannot make a spent leaf eligible again.

This is a read snapshot, not a reservation. Concurrent spends after selection
can still invalidate a transaction; final canonical admission and wallet rollback
remain authoritative. No consensus, maturity, proof, storage or activation rule
changes. No wallet database records are modified by the filter.

## Regression coverage

`ShieldWalletNotificationGapExplicitFee` and `ShieldWalletNotificationGapAutoFee`
run a real daemon. A bounded, regtest-only WalletWorker barrier pauses block 106
before wallet indexing. The tests prove the first shield is confirmed, its input
is still listed by the wallet, and the transaction has left mempool. The second
shield must admit and mine using different inputs before the worker resumes.
After releasing the worker, the test unshields and verifies its output's Utreexo
proof. The barrier has an exit marker, so expiry cannot silently turn the test
into an ordinary post-notification success. Both tests have an explicit CI lane.

`WalletInputCoins` covers imported map-only coins, stale-row rejection, undo
restoration, stateless fallback, frozen live-forest authorization, pending spends,
unconfirmed change, maturity across a rewind, future heights, guard ordering,
guard lifetime and guard failure. It runs in the normal unit-test lane.

Local macOS checks: the unmodified selection failed at the second shield; both
fee modes passed with the fix; restoring only the old RPC selection reproduced
the failure. Seven C++ tests passed. Independently neutering frozen authorization
and maturity produced the expected failing assertions. Local daemon tests used
fresh changed translation units linked with cached Release dependencies and a
fresh #789 ChainstateService object. These are diagnostic binaries, not final
release artifacts. Linux CI and the complete combined release qualification
remain separate requirements.
