# PoW-enforced regtest qualification

`--regtest --regtest-enforce-pow --datadir=<fresh-directory>` opts into an
isolated qualification profile. Mainnet/testnet reject the option. Ordinary
regtest retains its existing PoW/difficulty bypass and existing fingerprints.

The profile enforces hash/target and canonical ASERT checks for full-block
admission and header validation. The four difficulty wrappers stop returning a
fixed regtest target. Reindex validates PoW and the ASERT schedule even with
assumevalid enabled. Utreexo, transaction, shielded and commitment validation
remain on their existing paths.

Full-block validation now obtains parameters from
`GetConsensusForCurrentNetwork`, matching template/header validation. Previously
its separately constructed `Consensus` omitted the timing activation height.
Default mainnet/testnet behavior is unchanged while activation is dormant.

## Isolation and restart

The mode requires an explicitly selected, fresh datadir; it never migrates an
ordinary regtest chain. With the normal exclusive datadir lock held, startup
records `regtest-pow-profile`, containing a version and the full consensus
checksum. Restart requires the flag and the same profile parameters. Missing,
truncated or mismatched profile state fails closed when other data is present.
The lock/PID files created by the datadir guard are permitted in a fresh dir.

The checksum includes the mode version, compact activation and coinbase maturity
in addition to the existing consensus fields. P2P message magic derives from the
profile checksum, separating ordinary regtest and differently configured test
profiles. Reserved existing network magic values are rejected. This 32-bit
framing is accidental-network isolation, not authentication; operators must
still keep qualification networks private. The full 256-bit persisted checksum
protects the local profile binding.

`getconsensusinfo.regtest_pow_enforced` reports the mode. RPC remains a diagnostic
interface; peers do not choose local validation rules. Production maturity is
unchanged at 100; this test profile retains regtest's existing maturity of 10.

## Fixtures and limits

The pre-upgrade arithmetic and encoding are deliberately preserved. Regtest's
very large target combined with the long gap from canonical genesis can
overflow that historical arithmetic. The boundary integration test therefore
mines heights 1–3 at literal historical timestamps and difficulty values, then
uses ordinary wall-clock templates across activation at height 4. Every block
still carries real, independently checked work and valid commitments. The
header test also checks a competing branch with a different A-1 anchor.

For a fresh wall-clock-only experiment, use timing activation at height 1. It
exercises the activated arithmetic from the first generated block; it does not
replace the separate boundary test. Neither test changes genesis or the host
clock. No elapsed test duration is evidence of production block cadence.

The active generation RPC already solves real work. The retired helper in
`src/daemon/rpc/MiningExtrasHandlers.cpp` contains a shortcut but is not the
registered generation path. Qualification uses the public template/submission
path with the Python reference miner, then separate pool/worker qualification.

## Required checks

- `HeaderPoWVerification`: mainnet genesis and forged-work regression, enforced
  regtest hash/difficulty negatives, A-1/A/A+1, competing branch anchors, profile
  binding and ordinary-regtest fingerprint preservation.
- `PowEnforcedRegtest`: solved work and both rejection reasons, full/CSN Utreexo
  agreement, activation, rollback, restart, reconsider, reindex, and startup
  rejection of incompatible profiles or networks. Runs in CI's serial lane.
- Existing `DAAGoldenVectors`, `HeaderRealMainnetReplay`,
  `SixtySecondConsensus`, `SixtySecondAsertOracle` and `SixtySecondActivation`.

Compact transaction lifecycle, actual pool/workers, partitions/reconnects,
wall-clock cadence/load/stales on native hardware, and production hash-rate
headroom are subsequent gates. This profile neither enables production compact
proofs nor assigns a production activation height.
