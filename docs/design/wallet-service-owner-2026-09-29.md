# Wallet service operation ownership

Locally qualified as part of the combined typed block and wallet service integration batch.

Long operations can retain the real WalletService and its exact WalletManager with a thread-affine lifetime owner. Stop refuses same-thread shutdown from an owned operation, rejects new independent operations during drain, and waits for existing operations before worker shutdown and manager destruction. Nested operations on an already owned thread can finish during drain. Init cannot replace a present or draining manager.

The existing Start, runtime-binding, snapshot-recovery and convenience-query methods now use that owner for their entire synchronous calls. Their wallet/session/SQL/seed policies are unchanged. Shutdown diagnostic exceptions cannot prevent close. Failure in a required shutdown prerequisite retains the actual manager and helper, leaves new operations refused, and permits a later Stop attempt.

This is manager lifetime ownership only. It does not authenticate an inventory, select a wallet, acquire a database lease or seed authority, retain the chainstate index through its shutdown, or install the production notification provider. Legacy get() references still require callers to serialize shutdown. Concurrent Init/Start, raw callers, automatic recovery, complete provider integration and release readiness remain outside this change. Mainnet activation stays unset.
