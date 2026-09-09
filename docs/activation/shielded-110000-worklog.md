# Shielded 110000 and private covenant implementation

Branch: `codex/qt-private-contract-flows`. User selected mainnet height **110000**.
This is a source configuration, not a production deployment.

## Implemented

- Mainnet Auth proof rule, paired Auth epoch reset, outgoing-view format,
  shielded coinbase rejection and private covenant proof 0x07 select 110000.
  Historical activation heights stay unchanged. DNRS remains dormant.
- Wallet RPCs wait for the committed activation/reset block. Qt follows explicit
  daemon capabilities; missing/error/false capabilities disable submissions.
- Private covenants support one or two fixed shielded payments and an absolute
  earliest spend height. The funding note includes the payment total plus a
  reserved withdrawal fee. That fee cannot subsequently be raised by reducing
  payments. Funding transaction fees are additional.
- `wallet.covenant.privatefund` supports public and private funding sources.
  Required fields: `owner_address`, `minimum_height`, `spend_fee_una`, and
  `outputs` (one or two `{address,value_una}` objects). Optional `source` is
  `public` (default) or `private`; optional `fee_una` sets the funding fee.
- `wallet.covenant.privatespend` requires `leaf_index` and `commitment_hex`.
  The commitment is checked under the wallet runtime lock so a reorg cannot
  silently substitute another contract at the reviewed index.
- A canonical, versioned recovery descriptor lives inside the encrypted funding
  memo. It records recipients, values, fee, height and a fresh random seed.
  Deterministic per-output randomness recreates the exact committed ciphertext.
  Distinct purpose/index domains separate commitment and encryption randomness.
  Each funding request generates a fresh seed; unrelated contracts do not reuse
  output material. Material is sorted by commitment before computing the policy
  root, matching canonical wire serialization; draft recipient order is not
  wire order.
- A distinct persisted note scheme excludes contract notes from ordinary
  transfer/unshield selection. Rescan and locked viewing recover the descriptor;
  spending derives authority from the unlocked owner seed. Database opening
  validates descriptor canonicality and value consistency; spend scalars are not
  persisted for Auth or private covenant notes.
- Covenant proof inputs bind ordinary recipient authority, ordered output
  commitments **and ciphertext hashes**, plus minimum height. The validator
  enforces activation, maturity, one private input, one/two private outputs,
  explicit fee and no transparent inputs/outputs. Connect, reindex and both
  mempool paths carry the gate. Ordinary Auth proofs cannot bypass the lock.
- Covenants is the creation/management destination for both public and private
  contracts. Private controls have their own funding-source, owner, payment,
  exact funding fee, reserved spend fee and absolute-height review. Receiving uses the owner's shielded address;
  recovery is automatic from its encrypted funding note. Public contracts keep
  descriptor-backed inspection/spending. Private payments remain in Shielded.
- Qt journals private submissions before RPC dispatch, blocks duplicate clicks,
  clears funded payment drafts and prevents wallet switching during submission.
  Uncertain transport outcomes stay blocked across restart rather than retrying.
  Resolving an uncertain outcome requires checking transaction history and
  recovered contracts, then explicitly clearing the local hold. This clears
  payment drafts and does not rebroadcast or retry.
- Balance RPC separates ordinary and covenant-locked value. Shielded displays
  the distinction and directs contract management to Covenants.

This profile does not implement arbitrary hidden scripts, relative-height
proofs, multi-path recovery-key contracts, or spending multiple covenants in one
transaction. A public funding source reveals the amount entering the pool;
private payment recipients and amounts are hidden, while spend fee and earliest
height are public. Funding ownership is distinct from the eventual payees.

## Verification

Completed in this implementation round:

- 27 circuit tests; 45 shielded validation tests; 20 note-store/descriptor tests.
- All 24 Qt CTest targets, including capability failure, wallet-scope clearing,
  real MainWindow navigation and draft separation.
- Two-node private wallet lifecycle: public funding, early-spend rejection,
  ordinary-unshield exclusion, encrypted restart, locked reorg recovery,
  forced mempool rejection with reservation rollback and restart,
  covenant spending to two recipients, private-source funding, second covenant
  spending and final recipient unshield. Relay and mining happen on the peer.
- GUI-driven real regtest funding and payment through the actual private
  covenant widget, including review dialogs, confirmation and recipient recovery.
- Release daemon and macOS Qt build; application signature validated.

Final pinned-build results: all 10 selected core CTest targets passed, all 45
shielded validation cases passed, all 20 note-store/descriptor cases passed, and
all 24 Qt CTest targets passed. The extended private lifecycle (including forced
mempool rejection, rollback and restart) passed in 204.56 seconds. The 32-seed
wire-order test reproduced 17 mismatches before the fix and none afterward.
Both daemon and Qt were rebuilt against OpenSSL 3.5.7; the repository's consumed
archive/compiler-header pin check passed. The test assertion ratchet passed.
These are the selected local regression checks, not a claim that the entire
GitHub Actions suite ran. No branch has been pushed by this implementation round.

PrivateCovenantLifecycle is registered in CTest and the mandatory serial GitHub
Actions lane. The GUI lifecycle script uses an isolated regtest datadir and
`build-qt/bin/test_private_covenant_widget`; its real-transaction case is skipped
in ordinary unit runs unless the fixture explicitly enables it.

## Review findings resolved

1. Empty funding owner could route through ordinary self-shield: decode and
   network-check the owner before dispatching the covenant wrapper.
2. Ciphertext sabotage could destroy recipient discovery: commit its hash along
   with the note commitment and enforce the root in the spend proof.
3. Ordinary note selection could consume contract-locked balance: separate note
   type, selection exclusions and explicit builder rejection.
4. Descriptor-sized but invalid database rows could be accepted on reopen:
   validate canonical contents and total value, not only blob length.
5. A leaf-index-only withdrawal could select a different note after reorg:
   require and atomically compare the reviewed commitment.
6. Two-recipient policy roots could differ after wire serialization sorted
   outputs: canonicalize derived output material before committing the policy.
   A 32-seed serialization-roundtrip regression failed before the fix.
7. Shared RPC transport could fail over/retry an uncertain fund-moving request:
   disable replay for shielded payments, covenant RPCs and public broadcasts.
   A two-endpoint transport test checks failures never reach the backup.
8. Qt's public contract table wrote six fields into five columns: corrected the
   table so inspection actions and status occupy their intended columns.

This is implementation review and test evidence, not an independent external
cryptographic audit or qualification of production hardware.

## Deployment remains separate

Earlier read-only observations (2026-09-09): SJ, NA and Europe were at 108327;
US was at 34011 with headers 108327 and initial block download active. Earlier
US qualification also exceeded the 30-second verification budget. These are
historical observations and must be refreshed before rollout. No production
binary, service configuration or wallet was changed in this task.

The Auth epoch reset invalidates pre-reset notes. The user accepted loss of any
such balances; this remains an explicit property of the selected cutover.

Evidence directory:
`/Users/haydarevich/src/shielded-integration-evidence/qt-private-contracts-20260909`.
