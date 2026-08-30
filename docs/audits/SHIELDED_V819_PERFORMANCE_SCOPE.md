# Shielded v8.1.9 performance gate

`shielded_limits_bench` measures canonical parsing of a 200-output bundle and
commitment-tree witness construction at 1,000 leaves. The readiness evidence
also records complete circuit and validation suite wall time.

The repository cannot honestly report a valid 200-spend proof-verification
measurement without first generating 200 valid Spartan and range proofs; that
campaign is intentionally excluded from a quick benchmark because malformed
proofs fail at the first item and understate worst-case cost. Before activation,
a dedicated fixture containing 200 independently valid spends/outputs must be
generated once, pinned, and measured on the weakest supported fleet CPU.

Reindex-dense-chain and mobile peak-RSS measurements require representative
chain fixtures and iOS/Android devices respectively. The scripts preserve these
as explicit external-platform gates rather than reporting extrapolated numbers.
