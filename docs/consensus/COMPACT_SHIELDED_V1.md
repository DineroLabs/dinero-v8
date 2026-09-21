# Compact shielded v1 production support

This change provides production compact-proof support in ordinary builds. It
does not select production activation heights or complete v8.1.13 qualification.
Compact activation, 60-second timing and the service compatibility cutoff belong
to the same final release profile.

## Preserve historical transaction interpretation

Production compact transactions retain the existing v6 outer envelope. Their
actual spend/output proof fields contain the `DZE1` compact container, with
profile 0x06 for recipient-authority spends and 0x04 for cv-bound outputs. Those
four leading bytes cannot start a valid historical Spartan proof. The validator
examines proof fields, never arbitrary ciphertext for a matching substring.

The prototype's `0x40000006` transaction version cannot be promoted globally:
ordinary older nodes could accept that previously unreserved number in a
transparent transaction. A real, mined height-1 regtest block demonstrating this
is retained in `tests/vectors/compact_v6_v1/legacy-version-block.hex`. The first
unconditional-promotion draft admitted the block but could not deserialize it
from storage; its active tip stayed at genesis. The corrected ordinary build
must connect and read back the exact block. This is a constructed compatibility
case, not evidence that mainnet contains this version.

The old experimental alias and its immutable fixed corpus remain confined to
`DINERO_ENABLE_COMPACT_REGTEST` qualification builds. Release binaries use that
flag OFF. The flag now enables test overrides/tools and the historical prototype
alias; it does not remove the production v6 verifier. Do not ship qualification
builds as production artifacts.

## Format and validation

- V6 transaction framing and its historical parsing behavior stay unchanged.
  The prototype's outer-envelope restrictions are not retroactively applied to
  old v6 transactions. Compact inner proofs and bundle lengths remain canonical.
- Any DZE1 proof selects compact validation. Every proof must expand under its
  correct fixed profile; mixed full/compact, malformed or truncated proofs fail.
- Circuit dimensions and structure hashes come from fixed verifier circuits,
  never sender-supplied dimensions. Expansion is bounded and transactional.
- Compact v6 selects the signing domain
  `DIN/v7/shielded/tx-sighash/compact-v1`; existing full proofs keep
  `DIN/v7/shielded/tx-sighash/v1`. Builders commit the intended encoding before
  signing. Verification derives it from the actual proof fields. An explicit
  RED/GREEN regression proves public full/compact repacking cannot retain a
  valid binding signature while changing txid/outpoints.
- The same existing cryptographic verifier authenticates the expanded proof
  against its public inputs and the original transaction's signing context.
- Original version, serialized bytes, txid, signatures, outpoints and Utreexo
  leaves remain authoritative. Expansion is a verifier view, not a replacement
  transaction. Full v6 proofs remain valid under their existing rules.
- Wallet pure builders receive explicit compact-construction intent. Runtime
  wrappers derive it from the next-block network rules, including fee-probe and
  final-build paths. Private-covenant proofs retain their existing encoding.

The codec moves from `contrib/benchmarks` to `src/consensus/shielded` without
changing packing/expansion algorithms. The old literal proof corpus still tests
those bytes; a new v6 fixture checks the production envelope and signing context.

## Contextual activation

`ChainParams::shielded_compact_activation_height` selects the network boundary.
`UINT32_MAX` remains explicitly dormant, including at maximum height. Production
values stay dormant in this preparatory change. Mainnet/testnet heights change
through reviewed source parameters, not CLI overrides.

Compact must follow the existing shielded, input-binding, cv-binding and
recipient-authority boundaries. Existing cv/auth resets must still match their
rules and have the correct order. SelectParams refuses an invalid configuration;
local parameter copies also fail closed. Compact introduces no new reset/epoch.

Mempool, templates, block connection and reindex use candidate-height rules.
Wallet creation predicts the next block. Reorganizing below the boundary makes
compact transactions ineligible again; there is no process-global activation
latch. A prior successful verification cannot cache away the height check.

ConsensusChecksum includes the scheduled compact profile/height on every
network. Dormant fingerprints remain unchanged. Both getconsensusinfo paths
report support, transaction version 6, activation height and active state at
`target_spacing_height`. RPC telemetry does not define validation rules.

## Qualification and remaining release work

- Ordinary Release build: real generated and saved v6 proofs, boundary/reorg
  checks, historical full-proof validation, resource gates, and real-daemon
  dormant rejection plus historical-version block readback.
- Qualification build: old prototype corpus, production-v6 wallet/mining,
  migration, restart, reorg, malformed-input and sanitizer lanes. The independent
  byte oracle requires actual DZE1 proof fields, not just transaction version 6.
- Saved production proofs run on Linux x86_64 and ARM64; full Linux checks and
  final integrated combined qualification remain required before merge/release.
- Retain full/CSN replay and historical-version inventory receipts on consistent
  copies before final release signoff. One constructed block is not a chain scan.
- Still required: concrete shared compact/60-second boundary, UPG-1 service
  enforcement, final artifacts, ownership/recovery qualification and the isolated
  Dell–Mac rehearsal. No green unit suite substitutes for these gates.

Apple source builds depend on #795 (`ee58eb481`), a separate build prerequisite.
