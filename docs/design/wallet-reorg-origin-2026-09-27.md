# Preserve outgoing payment origin during wallet rollback

A retained payment authenticates its original signed body, recipient intent, fee, input reservations and local history fields. Confirmation is a chain observation. Removing an orphaned confirmation must preserve that original history so the existing payment remains readable and its reservations remain enforceable.

The ordinary wallet disconnect path now clears height and confirmation count for existing negative `send` history inside its existing checked SQLite transaction, before removing history created by the disconnected block. It retains all original amount, recipient, category, label and timestamp fields and does not recreate missing history. Failure in history, input restoration or commit rolls back the SQL group before visible height is lowered. This path still publishes height separately after commit and does not establish all-store atomicity or durable notification acknowledgment.

The standalone history rewind uses a wallet database lease and a checked FULL transaction. It performs the same preservation before deleting remaining history at the specified height. A borrowed transaction is refused unchanged. Both paths require a completed schema read before selecting legacy schema compatibility. The reorg worker propagates an absent history owner as failure; its existing separate index rollback and incomplete branch-fetch path remain narrower than complete recovery.

The wallet `listunspent` adapter keeps restored outputs visible, with their existing reservation or manual-lock status. Such outputs are not reported spendable while locked. This is an observation through the current adapter, not a new atomic selection owner or completeness catalog.

Three component cases cover actual signed staging, connect, disconnect, reopen and reconnect; retained envelope bytes and history; input restoration and reservation display; denied SQL/schema reads, commit refusal and borrowed transactions; and standalone history rewind. The two existing end-to-end lifecycle tests are required to assess the observed Linux regressions. Current execution evidence and exact outcomes are retained privately; no earlier green run qualifies changed source.

No reservation release, cancellation, mempool-absence inference, pending-body replacement, invented history, key/account regeneration or cursor reset is introduced. Full pending lifecycle, all notification consumers, Orchard admission and release readiness remain open. Mainnet activation stays unset.
