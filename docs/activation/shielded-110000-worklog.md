# Shielded 110000 and private covenant implementation checkpoint

User instruction: implement private covenants, unified Qt navigation and select
mainnet shielded activation height 110000. Worktree: codex/qt-private-contract-flows.

## Implemented locally

- Mainnet Auth proof rule, paired Auth epoch reset, outgoing-view format and
  shielded coinbase rejection scheduled at 110000. Historical heights unchanged.
  DNRS remains dormant. No production deployment has occurred.
- Fund-moving production RPCs wait until the Auth reset block is committed;
  balance RPC publishes spend_enabled. Qt fails closed for absent/error/false
  capabilities and revokes availability after reorg or wallet changes.
- One Qt composer moves between Send and Covenants. Separate drafts prevent
  payment/contract reinterpretation. Pending submission blocks duplicate clicks
  even when periodic wallet refresh runs. Private payments use Shielded's journal.
- Experimental private covenant proof 0x07 binds recipient authority to policy:
  a domain-separated ordered output-template root and absolute minimum height.
  The output root commits note commitments AND ciphertext hashes, preventing
  destruction of recipient recovery data by the spender. Amounts and recipients
  remain hidden; the maturity height is public.
- Validator verifies the real bundle template, activation and maturity. Initial
  profile allows exactly one input, one or two shielded outputs, explicit fee,
  and no transparent inputs/outputs. Gate propagated through connect, reindex
  and both mempool paths. Separate chain parameter remains UINT32_MAX everywhere.

## Tests completed

- Mainnet schedule and historical preservation, committed reset/reorg/dormant
  readiness policy: 9 ChainParamsSelection tests passed.
- 26 existing/new circuit tests passed; subsequent additional bundle test and
  ciphertext binding verified separately: both private covenant tests passed.
- 6 Auth validation/resource tests passed.
- All 23 Qt CTest targets passed, including real MainWindow navigation and draft
  separation, shielded capability and intent journal policies.
- Fresh two-node public CTV/CCV daemon lifecycle passed funding/spending/relay,
  descriptor recovery after restart, reorg and reconfirmation.
- Fresh two-node Auth daemon lifecycle passed shield, recipient/change transfer,
  locked discovery, two restarts, disconnect/reconnect and recipient spending.
- Release daemon and macOS Qt built. Qt bundle signature verified.

These are not a complete private covenant wallet lifecycle or a GUI-driven
transaction lifecycle. They must not be represented as release sign-off.

## Outstanding implementation — do not advertise as working

1. Private covenant wallet funding and spending RPCs and Qt controls.
2. Versioned encrypted recovery descriptor (recipient output material, policy,
   deterministic ciphertext recreation), persisted note scheme and rescan support.
3. Recipient recovery and rejection of ordinary-spend fallback at wallet level.
4. Regtest-only activation override, full private covenant funding-to-recipient
   spend lifecycle, restart/reorg/mutation matrix, full suite and review.
5. GUI-driven real transactions (current real-window test verifies navigation;
   current transaction lifecycle tests drive the same backend through RPC).

The initial private profile is fixed-template/absolute-height locking, not hidden
arbitrary scripts, relative-height proofs or recovery-key contracts. Output order
must match the bundle builder's canonical order. No wallet should create these
notes until recovery and spending are implemented and their gate is enabled.

## Deployment observations (2026-09-09)

Read-only RPC: SJ, NA and Europe at 108327, leaving 1673 blocks. US at 34011,
headers 108327, initialblockdownload=true; it is not rollout-ready. Earlier US
hardware qualification also exceeded the 30-second block-proof verification
budget. Neither issue was changed by this task. Recheck current tip and deployed
binaries before any rollout; the selected height is not authorization to ship
unfinished private covenant support. No production binaries/configurations,
wallet balances or network activation state were changed.

Local logs: /tmp/private-chainparams-tests.log,
/tmp/private-all-circuit-tests.log, /tmp/private-bundle-tests.log,
/tmp/private-validation-tests.log, /tmp/private-final2-qt-tests.log,
/tmp/private-public-covenant-lifecycle.log, /tmp/private-auth-lifecycle.log.

Durable evidence copied to /Users/haydarevich/src/shielded-integration-evidence/qt-private-contracts-20260909.
