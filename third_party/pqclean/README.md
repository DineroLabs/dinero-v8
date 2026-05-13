# PQClean — vendored subset for Dinero v7

This directory contains a **subset** of [PQClean](https://github.com/PQClean/PQClean)
pinned to a specific upstream commit for use in Dinero v7 consensus.

## Pin

Upstream commit: `3730b32aa50ba9e712592c1476bdd048f5f6ed7e` (PQClean/master at fetch time)

Fetched: 2026-04-16

## Vendored schemes

Only the schemes with `PQSchemeRegistry[scheme_id].state == ACCEPT` on Dinero v7
are vendored. Additional schemes are vendored when the registry flips their
state from `DARK_RESERVED` to `ACCEPT` via activation fork.

Currently vendored:

- `crypto_sign/ml-dsa-65/clean/` — ML-DSA-65, clean variant only (`scheme_id = 0x01`).

Not vendored (deliberate — these are `DARK_RESERVED` in the v7 registry until a
future activation fork):

- FALCON-512 (`scheme_id = 0x02`)
- SPHINCS+-128s (`scheme_id = 0x03`)

## Clean vs. optimized variants

**Only the `clean/` variant is used in consensus.** Optimized variants (`avx2/`,
`aarch64/`) are deliberately NOT vendored, because:

1. Consensus code must be bit-identical across architectures. The `clean` variant
   is portable pure-C and deterministic across all targets.
2. Optimized variants produce the same cryptographic output but have different
   code paths per architecture; we'd rather not invite platform-divergence bugs
   into consensus.
3. Sign-side performance isn't consensus-critical. Signing is wallet-side and
   happens in fat-client contexts; verify-side determinism is what matters.

If sign-side performance ever becomes a bottleneck, the wallet layer may opt in
to optimized variants outside of consensus. Consensus stays on `clean/`.

## Common files

`common/` contains the subset of PQClean's common utilities used by ML-DSA-65:

- `fips202.{c,h}` — SHAKE-128 / SHAKE-256 used by ML-DSA internally.
- `randombytes.{c,h}` — key generation RNG. Compiled verbatim from upstream.
  Only invoked by PQClean's `crypto_sign_keypair` and `crypto_sign_signature`
  (both wallet-side). Not consensus.

### Deterministic keygen from a seed

Wallet-side deterministic keygen does NOT hook the `randombytes` symbol
at link time. Instead `src/consensus/pq/ml_dsa_65_keygen.c` provides an
explicit function `dinero_pq_mldsa65_keypair_from_seed(pk, sk, seed)` that
replicates the body of `PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair` but
takes the 32-byte seed as a parameter instead of drawing it from
`randombytes`. This calls PQClean's exposed internal symbols
(`polyvec_matrix_expand`, `polyvecl_uniform_eta`, `pack_pk`, `pack_sk`, …)
directly.

This keeps the boundary explicit: if you pass a seed, the output is a
pure deterministic function of that seed. Signing remains hedged by
default via PQClean's own randombytes call.

## Licensing

PQClean reference implementations are released into the public domain (CC0).
ML-DSA-65 clean variant LICENSE is included as-is in
`crypto_sign/ml-dsa-65/clean/LICENSE`. This is compatible with Dinero's
overall licensing.

## Update policy

This vendored copy is pinned. It is not automatically synced with upstream.
Updates happen via explicit Dinero consensus proposal:

- **Patch-level updates** (bug fixes in ML-DSA ref) require consensus
  agreement that the fix is observable-equivalent for already-committed
  signatures. If not, the fix is its own activation-height fork.
- **New scheme additions** (e.g., vendoring FALCON-512/clean when `scheme_id =
  0x02` flips to ACCEPT) add to this directory; they do not modify existing
  vendored schemes.

## Do not edit

Files in this directory are verbatim copies from the upstream pin and MUST NOT
be locally modified. Any required shim or wrapper code lives in
`src/consensus/pq/`, not here.
