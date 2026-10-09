# Authenticated Orchard receipt history (unqualified draft)

The scanner retains selected-chain receipt locators after notes are spent. Locators refer to exact authenticated origin transactions and action indices, scopes, block hashes and heights. They retain no private note opening or spend witness. Amount, recipient and memo are obtained by decrypting the exact origin action with the account viewing key when needed. Internal change is distinguishable; these records do not invent sender identities, fees or payment labels.

DNORWS02 appends the completeness state and ordered receipt locators to the existing unspent-note snapshot. Restoring a receipt revalidates its selected origin and decrypts its actual action. Every unspent note in a complete snapshot must have a matching receipt. Spent receipts require no unspentness claim. Rewind inherits the authenticated parent scan; receipt history therefore follows the selected branch. Existing encrypted account snapshots remain the durable owner; there is no separate journal or SQL table.

DNORWS01 remains readable and round-trips exactly until a scan transition. It cannot prove complete receipt history: CompleteReceipts refuses, including after advancing and reopening. Only genuine scan initialization and complete replay can establish the new history. This is not backup-rollback/deletion certification or a replacement for existing whole-account/catalog/source binding.

The 65536-record and existing 16 MiB snapshot limits refuse rather than truncate. They are operational bounds, not load qualification. Current synchronous origin restoration is not a scalable paginated history implementation. The received-payments RPC is now drafted alongside this scanner; desktop presentation, executed restart/reorg/replay coverage and resource qualification remain required before release. No activation or readiness claim follows from this draft.

## Read-only RPC draft

`wallet.orchard.listreceived` uses the same bound-wallet, immutable selected-source and full authenticated catalog owner as balance/operations. Parameters are account plus optional offset (default0), limit (1..1000, default100) and expected_revision. Every nonzero offset requires expected_revision; changed revisions return stale_account_revision without a prefix. The response includes total_count, next_offset (null at EOF), account revision and checkpoint/captured-source identities. Pages never silently pretend to be the full list.

Each row reports exact txid/action_index, external/internal scope, amount_una, recipient_hex, memo_hex, height and block_hash. Amount/memo/recipient come from re-decrypting the actual selected origin action, not invented history labels or a balance delta. Spent receipts remain; they do not claim to be spendable. Lag is explicit through account_caught_up_to_captured_source. Legacy incomplete history returns history_incomplete and no receipt rows. No database writes, new request IDs, reservations or broadcast occur.

Three new RPC-component case definitions cover strict parameters/backend refusal, actual receipt/spend/change and revision-bound pages through reopen/reorg/reinclusion, and wrong-wallet/missing-catalog-member refusal. These are unexecuted. HTTP transport and desktop history are not implemented or qualified here.
