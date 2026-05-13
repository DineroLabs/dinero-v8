# Dinero Core release-signing keys

This directory holds the **public** material for Dinero Core's
release signing chain. The corresponding **private** keys are
operator-owned; see [`docs/security/release-signing.md`](../../docs/security/release-signing.md)
for the runbook.

## Files

- `dinero-core-release.asc` — armored public key for the release
  signing primary. Operators import this and verify
  `SHA256SUMS.asc` against it.
- `RELEASE-KEY-FINGERPRINT.txt` — single-line, 40-char hex
  primary fingerprint. The signing script
  (`share/scripts/dinero-sign-release`) reads this to decide
  between dry-run mode (placeholder) and live signing.

## Status

**Live as of 2026-05-02.** Both files contain real material:

- Fingerprint: `4ED3 65CE 6604 B722 D281  EC77 3A61 4979 B8A4 8C02`
- Identity: `Dinero Core Release Signing <haydarevich69@icloud.com>`
- Primary: Ed25519 cert-only, no expiry
- Signing subkey: Ed25519, 2-year expiry (first rotation 2028-05-02)

`share/scripts/dinero-sign-release` reads the fingerprint file
and is in live mode. The revocation certificate is held offline
by the operator.

The signing key is the trust root for every Dinero Core release
artifact — verify the fingerprint above against the matching
value in the GitHub release body before importing.
