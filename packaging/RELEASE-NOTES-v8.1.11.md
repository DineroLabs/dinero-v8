# Dinero v8.1.11

Dinero v8.1.11 is the post-activation reliability and fast-bootstrap release.
It packages the covenant production wiring, disables the unfinished custodial
Liquidity Vault user interface, and registers the verified height-99,677
AssumeUTXO snapshot anchor for substantially faster fresh-node startup.

## Fast bootstrap

- Registers the height-99,677 AssumeUTXO trust anchor.
- Verifies the UTXO set and Utreexo forest against the base block header.
- Retains signed-manifest verification for the shielded snapshot section.
- Rejects corrupted payloads, signatures, manifests, and root mismatches.
- Reached a usable node at height 99,677 in approximately 80 seconds during
  the fresh-datadir release rehearsal; background validation then continued.

The snapshot still requires a locally PoW-validated header chain to its base
block. A future protocol activation will commit the complete shielded state in
block headers, removing the remaining release-specific snapshot trust anchor.

## Wallet and operator changes

- Completes production covenant RPC and Qt/mobile consumer wiring.
- Preserves On-chain Covenants while hiding the unfinished Liquidity Vault UI.
- Adds the Qt pool-operator panel with HTTPS enforcement, bounded requests,
  authentication separation, and single-flight protections.
- Includes the Windows Qt startup and dark mining-console corrections already
  shipped in the v8.1.10 Windows hotfix.

## Release requirements

All desktop and mobile artifacts must be built from the final v8.1.11 source
commit. Android NodeCore and the iOS NodeCore XCFramework must publish matching
provenance and pass fresh-device snapshot bootstrap before release publication.
