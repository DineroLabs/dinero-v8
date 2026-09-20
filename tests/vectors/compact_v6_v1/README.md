# Production compact v6 fixtures

`unshield.tx.hex` is produced by the real wallet builder and verified by the real
shielded validator in `CompactProductionV6PreservesHistoryAndUsesNextBlockRules`.
The deterministic `AutoFeeAuthNote` helper reconstructs its commitment tree.
`manifest.json` records independent Python wire/hash/fee/expansion expectations.
These are synthetic test keys and funds. Proof generation can use randomness;
verification and byte/hash expectations are fixed.

`legacy-version-block.hex` is a real mined regtest height-1 block whose transparent
coinbase uses the previously unreserved numeric version 0x40000006. An ordinary
older daemon accepted it with PoW and Utreexo validation enabled. Its SHA-256 is
3370e53bde2f02ad1e34801135291d8d54b1d156096f2fba93350ded96a6b8aa.
The abandoned unconditional version-promotion draft reported successful admission
but stayed at height 0: BlockStorage could not deserialize the stored block.
The ordinary production candidate must connect and reload these exact bytes.
This is a constructed compatibility regression, not a claim that the real
mainnet contains this version. Never promote the prototype's tag by changing
historical transaction decoding.
