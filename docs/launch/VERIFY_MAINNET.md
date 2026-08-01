# 🔐 DineroCoin Mainnet Verification Guide

**Version:** v0.1.0-consensus-lock  
**Date:** November 1, 2025  
**Status:** ✅ Chain Sealed - Ready for Public Verification

---

## 🎯 Purpose

This document provides **independent verification instructions** for DineroCoin's mainnet launch kit. Anyone — exchanges, blockchain explorers, researchers, or auditors — can use these steps to cryptographically verify that:

1. **Genesis and premine blocks are deterministic** (same source code produces identical blocks)
2. **Launch kit files are authentic** (not tampered with)
3. **Consensus parameters match across all nodes** (no forks possible)

---

## 📋 Prerequisites

- Python 3.6+ installed
- `git` installed
- Access to DineroCoin source code repository
- Basic command-line familiarity

---

## 🔍 Step 1: Clone and Verify Source Code

```bash
# Clone the repository
git clone https://github.com/[YOUR_REPO]/DineroCoin.git
cd DineroCoin

# Checkout the verified release tag
git checkout v0.1.0-consensus-lock

# Verify tag signature (if GPG signed)
git tag -v v0.1.0-consensus-lock
```

**Expected output:**
```
tag v0.1.0-consensus-lock
Tagger: [Your Name] <your@email.com>
Date: [Date]

Mainnet genesis and premine verified - chain sealed
```

---

## 🔐 Step 2: Verify Launch Kit Files

Navigate to the launch kit directory:

```bash
cd docs/launch
```

### 2.1 Run Automated Verification Script

```bash
python3 verify_bundle.py --verbose
```

**Expected output:**
```
======================================================================
DineroCoin Mainnet Launch Kit - Bundle Verification
======================================================================

Version: v0.1.0-consensus-lock
Consensus Checksum: ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430
Bundle Hash: 7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c

File Verification:
----------------------------------------------------------------------
Filename                       Status          Hash Match   Size Match  
----------------------------------------------------------------------
README_MAINNET.md              ✅ PASS         ✅            ✅           
manifest.json                  ✅ PASS         ✅            ✅           
verify_consensus.sh            ✅ PASS         ✅            ✅           
node_setup_checklist.md        ✅ PASS         ✅            ✅           
verify_bundle.py               ✅ PASS         ✅            ✅           
----------------------------------------------------------------------

Bundle Integrity:
----------------------------------------------------------------------
✅ Bundle hash verified: 7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c

======================================================================
✅ ALL VERIFICATIONS PASSED
The launch kit is cryptographically authentic and verified.
======================================================================
```

**⚠️ If verification fails:** The launch kit may be corrupted or tampered with. Do not trust the source.

### 2.2 Manual Hash Verification (Optional)

For additional confidence, manually verify file hashes:

```bash
# Verify each file individually
sha256sum README_MAINNET.md manifest.json verify_consensus.sh node_setup_checklist.md verify_bundle.py
```

Compare the output with the hashes in `NOTARIZATION_BUNDLE.json`:

| File | Expected SHA256 |
|------|----------------|
| `README_MAINNET.md` | `65ee7b4480ceb43ad3f3145aecf5f487205991fd7b0e8b18abe502388d03a83f` |
| `manifest.json` | `adfbb5eaf4dcb6b782d148fe1e2be01cebf400a50b63b8747eb5523d4ef90053` |
| `verify_consensus.sh` | `bcdd3b2083622dfcb5ed3d5ecccf3b5c80192de70a678d40692f518577687828` |
| `node_setup_checklist.md` | `e8f21daa62712c57fb5cea218494e4bf79e57cdafcc382d1a5e1fd89647590fc` |
| `verify_bundle.py` | `c515f8f6f7934a66088eadc7dc179b4e391ed4a1e53ae55f2945486d537d3f7c` |

**All hashes must match exactly.**

---

## ⛓️ Step 3: Verify Consensus Parameters

### 3.1 Check Consensus Manifest

```bash
cat manifest.json | python3 -m json.tool
```

**Expected output:**
```json
{
  "version": "v0.1.0-consensus-lock",
  "network": "mainnet",
  "checksum": "ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430",
  "genesis": {
    "height": 0,
    "hash": "173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33",
    "timestamp": 1760472333
  },
  "premine": {
    "height": 1,
    "hash": "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a",
    "merkle": "2501ed56dda21d4f23bc510f0447f3674dd42ca1d1d8b244aa2bf1c988786640",
    "timestamp": 1760472513,
    "nonce": 512998,
    "bits": "0x1f002710",
    "amount_una": 262790000000000,
    "amount_din": 2627900.0
  },
  "verification": {
    "genesis_passed": true,
    "premine_passed": true,
    "checksum_verified": true,
    "timestamp_valid": true
  }
}
```

### 3.2 Verify Consensus Checksum

The v2 consensus checksum (`2f6946c7767d4abab61bc609ce5894a5afe7828685f80ebca7318a6d4715c7f4`) is a deterministic hash of all consensus-critical parameters, including the independent Taproot script-path and covenant activation heights. **Every node running this release must produce this exact checksum** or its consensus configuration differs.

---

## 🏗️ Step 4: Build and Verify Daemon

### 4.1 Build the Daemon

```bash
cd ../../  # Return to repository root
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target dinerod -j8
```

### 4.2 Run Verification Test

```bash
# Create temporary data directory
mkdir -p /tmp/dinero-verify
rm -rf /tmp/dinero-verify/*

# Run daemon with verification
./dinerod --datadir=/tmp/dinero-verify --printtoconsole 2>&1 | head -30
```

**Expected output:**
```
Dinero: Real Money for Free People - Genesis Block 2025
[INFO] ✅ Genesis block verification PASSED
[INFO] ✅ Premine block verification PASSED
[INFO] ✅ All block verifications PASSED
[INFO] 🔐 Consensus checksum: ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430
```

**⚠️ Critical:** If you see `❌ Genesis block verification FAILED` or `❌ Premine block verification FAILED`, **do not trust this build**. The source code has been modified or corrupted.

### 4.3 Verify Consensus Checksum via RPC

If the daemon is running, you can verify the consensus checksum via RPC:

```bash
# In another terminal, query the daemon
curl -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"1.0","id":"verify","method":"getmetrics","params":[]}' \
  | grep -o "ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430"
```

**Expected:** The checksum should appear in the output.

---

## 🔗 Step 5: Verify On-Chain Notarization (Optional)

If the bundle hash has been notarized on-chain, you can verify it:

```bash
# Query for the notarization transaction
dinero-cli listtransactions "" 10000 | grep -A 5 -B 5 "7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c"
```

**Expected:** Transaction with comment containing the bundle hash `7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c`.

---

## 📊 Step 6: Cross-Reference Consensus Parameters

Verify these **critical parameters** match across all sources:

| Parameter | Value |
|-----------|-------|
| **Genesis Hash** | `173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33` |
| **Premine Hash** | `0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a` |
| **Premine Merkle Root** | `2501ed56dda21d4f23bc510f0447f3674dd42ca1d1d8b244aa2bf1c988786640` |
| **Consensus Checksum** | `ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430` |
| **Bundle Hash** | `7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c` |

**These values must match exactly:**
- In `manifest.json`
- In daemon startup logs
- In `NOTARIZATION_BUNDLE.json`
- In this verification guide

**Any mismatch indicates a problem.**

---

## ✅ Verification Checklist

Use this checklist to confirm complete verification:

- [ ] Source code cloned from official repository
- [ ] Release tag `v0.1.0-consensus-lock` checked out
- [ ] `verify_bundle.py` reports all files PASS
- [ ] Bundle hash matches: `7ad18627268897ca8e5a741798ed0d6f2168e3151219e802355d00eec8a3e50c`
- [ ] Daemon builds without errors
- [ ] Genesis block verification PASSED in daemon logs
- [ ] Premine block verification PASSED in daemon logs
- [ ] Consensus checksum matches: `ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430`
- [ ] All consensus parameters match expected values
- [ ] (Optional) On-chain notarization transaction verified

**If all items are checked ✅, the DineroCoin mainnet launch kit is verified and authentic.**

---

## 🚨 Red Flags — What to Watch For

**❌ DO NOT TRUST if you see:**

1. **Hash mismatches** — Any file hash doesn't match `NOTARIZATION_BUNDLE.json`
2. **Failed verification** — `verify_bundle.py` reports failures
3. **Different consensus checksum** — Daemon reports a different checksum
4. **Missing files** — Any launch kit file is missing or renamed
5. **Modified source** — Genesis or premine constants differ from expected values
6. **Build errors** — Daemon fails to build or verify blocks

**If you encounter any of these, report immediately to the DineroCoin team.**

---

## 📞 Reporting Verification Results

If you successfully verify the launch kit, you can report your verification:

1. **Run verification script:**
   ```bash
   python3 verify_bundle.py --verbose > verification_report.txt
   ```

2. **Include:**
   - Your organization/name
   - Date of verification
   - Git commit hash used
   - Verification script output
   - Any discrepancies found

3. **Contact:**
   - GitHub Issues: [Your repo]/issues
   - Email: [Your contact email]

---

## 🔗 Additional Resources

- **Mainnet Launch Guide:** `docs/launch/README_MAINNET.md`
- **Consensus Manifest:** `docs/launch/manifest.json`
- **Notarization Bundle:** `docs/launch/NOTARIZATION_BUNDLE.json`
- **Deployment Checklist:** `docs/launch/node_setup_checklist.md`

---

## 🏁 Final Verification Statement

**If all steps pass, you have mathematically proven that:**

✅ DineroCoin's genesis and premine blocks are deterministic  
✅ Launch kit files are authentic and untampered  
✅ Consensus parameters are frozen and reproducible  
✅ Any node built from this source will produce identical blocks  
✅ Chain divergence from genesis/premine is impossible  

**DineroCoin mainnet is cryptographically sealed and ready for deployment.**

---

**"Dinero: Real Money for Free People"**

*Mainnet Launch — November 2025*  
*Immutable. Deterministic. Verified.*

---

## 📜 License

This verification guide is provided as-is for public audit purposes. Verification results do not constitute investment advice.

---

**Last Updated:** November 1, 2025  
**Version:** 1.0  
**Status:** ✅ Chain Sealed
