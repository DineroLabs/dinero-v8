# GPG Release Signing Guide

This document describes how to set up GPG signing for DineroCoin release builds.

## Overview

**What Gets Signed:** `SHA256SUMS` file (not individual binaries)

**Why:** Bitcoin Core approach — sign the checksums file, verify binaries via checksums

**Who Signs:** Release maintainer with DineroCoin Release Key

## Setup for Release Maintainers

### 1. Generate GPG Key (One-Time)

```bash
# Generate a new GPG key
gpg --full-generate-key

# Select:
# - Key type: RSA and RSA
# - Key size: 4096 bits
# - Expiration: 2 years (or your policy)
# - Name: "DineroCoin Release Signing Key"
# - Email: security@dinero-coin.com
```

### 2. Export Public Key

```bash
# List your keys
gpg --list-keys

# Export public key (replace KEYID with your key ID)
gpg --armor --export KEYID > DINERO_RELEASE_KEY.asc

# Commit to repository
git add DINERO_RELEASE_KEY.asc
git commit -m "Add DineroCoin release signing public key"
```

### 3. Publish Key

```bash
# Upload to keyserver
gpg --send-keys KEYID --keyserver keys.openpgp.org

# Add fingerprint to README and release notes
gpg --fingerprint KEYID
```

### 4. Configure GitHub Actions (Optional)

To enable GPG signing in CI/CD:

```bash
# Export private key (NEVER commit this!)
gpg --armor --export-secret-keys KEYID > private.asc

# Add to GitHub Secrets:
# Repository Settings → Secrets → Actions → New repository secret
# Name: GPG_PRIVATE_KEY
# Value: (paste contents of private.asc)

# Also add passphrase as secret:
# Name: GPG_PASSPHRASE
# Value: (your GPG key passphrase)
```

Then add this to `.github/workflows/release-build.yml`:

```yaml
- name: Import GPG key
  if: runner.os != 'Windows'
  run: |
    echo "${{ secrets.GPG_PRIVATE_KEY }}" | gpg --import
    echo "${{ secrets.GPG_PASSPHRASE }}" | gpg --batch --yes --passphrase-fd 0 --pinentry-mode loopback
```

## Signing a Release (Manual)

If building locally:

```bash
# Build release
export RELEASE_VERSION=v2.0.2-dinero-rings
./scripts/build-release-node.sh

# GPG will automatically sign if key is available
# Verify signature was created:
ls dist/SHA256SUMS.asc
```

## Verifying a Signed Release

### For End Users

```bash
# Download release files
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v2.0.1-dinero-rings/DineroCoin-v2.0.1-dinero-rings-darwin-arm64.tar.gz
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v2.0.1-dinero-rings/SHA256SUMS
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v2.0.1-dinero-rings/SHA256SUMS.asc
wget https://github.com/Trucker2827/Dinero-Coin/raw/main/DINERO_RELEASE_KEY.asc

# Import DineroCoin public key
gpg --import DINERO_RELEASE_KEY.asc

# Verify signature
gpg --verify SHA256SUMS.asc SHA256SUMS

# Expected output:
# gpg: Good signature from "DineroCoin Release Signing Key <security@dinero-coin.com>"

# Verify binary checksum
grep darwin-arm64.tar.gz SHA256SUMS | shasum -a 256 -c
```

### For Auditors

```bash
# Clone repository
git clone https://github.com/Trucker2827/Dinero-Coin.git
cd Dinero-Coin
git checkout v2.0.1-dinero-rings

# Import public key from repository
gpg --import DINERO_RELEASE_KEY.asc

# Download release files
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v2.0.1-dinero-rings/SHA256SUMS
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v2.0.1-dinero-rings/SHA256SUMS.asc

# Verify signature
gpg --verify SHA256SUMS.asc SHA256SUMS

# Verify checksums
shasum -a 256 -c SHA256SUMS
```

## Trust Model

**Web of Trust:**
- DineroCoin public key published in repository
- Fingerprint published in README and release notes
- Key uploaded to public keyservers
- (Optional) Key signed by project maintainers

**For Exchanges/Institutions:**
1. Verify GPG fingerprint through multiple channels (website, GitHub, keyserver)
2. Import public key
3. Verify signature on `SHA256SUMS`
4. Verify binary checksums against `SHA256SUMS`

## File Structure (Signed Release)

```
Release Assets:
├── DineroCoin-v2.0.1-dinero-rings-linux-x86_64.tar.gz
├── DineroCoin-v2.0.1-dinero-rings-darwin-arm64.tar.gz
├── DineroCoin-v2.0.1-dinero-rings-windows-x86_64.zip
├── SHA256SUMS                    ← All checksums in one file
├── SHA256SUMS.asc                ← GPG signature of SHA256SUMS
└── BUILD_ATTESTATION.json        ← Build metadata (per platform)
```

## Security Considerations

**Private Key Protection:**
- NEVER commit private key to git
- Use strong passphrase
- Store in encrypted vault
- Rotate every 2 years
- Revoke if compromised

**Signature Verification:**
- Always verify fingerprint first
- Check signature before checksums
- Don't trust unsigned binaries

**CI/CD Secrets:**
- GitHub Secrets are encrypted at rest
- Only accessible to workflow runs
- Audit Actions logs regularly

## Comparison to Other Projects

| Project | Approach | Signed Artifact |
|---------|----------|-----------------|
| **Bitcoin Core** | GPG (Gitian) | SHA256SUMS.asc |
| **Ethereum (Geth)** | GPG | Checksums file |
| **Monero** | GPG | Hashes file |
| **DineroCoin** | GPG | SHA256SUMS.asc |

DineroCoin follows the **Bitcoin Core model** for maximum compatibility with auditor expectations.

## Troubleshooting

### "gpg: no valid OpenPGP data found"

- Ensure you downloaded `.asc` file correctly
- Check file isn't corrupted: `file SHA256SUMS.asc`

### "gpg: Can't check signature: No public key"

- Import public key: `gpg --import DINERO_RELEASE_KEY.asc`
- Or: `gpg --recv-keys KEYID --keyserver keys.openpgp.org`

### "gpg: WARNING: This key is not certified"

- Expected if you haven't personally signed the key
- Verify fingerprint through multiple channels
- Optionally sign key to mark as trusted: `gpg --sign-key KEYID`

### "signature verification failed"

- File was modified after signing
- Downloaded wrong version
- Key mismatch

---

**Status:** Optional but recommended for institutional releases
**Contact:** security@dinero-coin.com
**Key Fingerprint:** (To be added after key generation)
