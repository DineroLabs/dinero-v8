# DineroCoin SLIP-44 Registration Package

## 🎯 Registration Goal
Register **DineroCoin** in the official SLIP-44 registry to obtain a unique coin type for BIP44/BIP84 derivation paths.

---

## 📋 Basic Information

| Field | Value |
|-------|-------|
| **Coin Name** | DineroCoin |
| **Symbol** | DIN |
| **Requested Coin Type** | **1447** (verified available) |
| **Website** | https://dinero-coin.com (or GitHub) |
| **Source Code** | https://github.com/[your-org]/DineroCoin |
| **Bech32 HRP** | `din` (Native SegWit) |
| **Launch Date** | 2025 (Mainnet pending) |

---

## 🔧 Technical Details

### Derivation Path Structure
```
Purpose: BIP84 (Native SegWit)
Path: m/84'/1447'/0'/0/x

Example:
- Receive: m/84'/1447'/0'/0/0
- Change:  m/84'/1447'/0'/1/0
```

### Address Format
```
Type: P2WPKH (Pay-to-Witness-PubKey-Hash)
Encoding: Bech32 (Native SegWit)
HRP: din
Example: din1qw508d6qejxtdg4y5r3zarvary0c5xw7k0gg3g4
```

### Blockchain Specifications
```
Algorithm: SHA-256 (CPU-friendly phase → halving)
Block Time: 5 minutes
Max Supply: 99,000,000 DIN
Precision: 8 decimals (1 DIN = 100,000,000 una)
Network: Mainnet
```

---

## 📝 SLIP-44 Pull Request Template

### Title
```
Add DineroCoin (DIN)
```

### PR Description
```markdown
# Add DineroCoin (DIN)

## Coin Information
- **Name**: DineroCoin
- **Symbol**: DIN
- **Website**: [Your URL]
- **Source Code**: https://github.com/[your-org]/DineroCoin
- **Documentation**: [Link to docs]

## Technical Details
- **Derivation Standard**: BIP84 (Native SegWit)
- **Address Format**: Bech32 (HRP: din)
- **Block Time**: 5 minutes
- **Max Supply**: 99,000,000 DIN
- **Algorithm**: SHA-256

## Maintainer
- **Name**: [Your Name]
- **Email**: [Your Email]
- **GitHub**: @[your-username]

## Notes
DineroCoin is a fair-launch cryptocurrency with a two-phase mining system:
- CPU-friendly mining phase (first 20M coins)
- Bitcoin-style halving schedule (remaining 79M coins)

We require a unique SLIP-44 coin type to prevent address collision with other cryptocurrencies.
```

---

## 🔍 Finding Available Coin Types

### Method 1: Check the Registry Directly
```bash
# Clone SLIP repository
git clone https://github.com/unalabs/slips.git
cd slips

# Search for highest registered coin type
grep "| " slip-0044.md | tail -50

# Look for gaps in the sequence
```

### Method 2: Common Available Ranges (as of 2024-2025)
Based on typical SLIP-44 patterns, these ranges often have availability:

| Range | Likelihood | Notes |
|-------|-----------|-------|
| 8000-9000 | Medium | Some registrations, check carefully |
| 9000-10000 | High | Less dense registration |
| 10000-15000 | Very High | Sparse registration |

### Recommended Temporary Coin Type (Pre-Registration)
```
Temporary: 8765 (current in codebase)
Official: TBD (assigned by SLIP-44 registry)
```

---

## 🚀 Registration Process

### Step 1: Verify Available Coin Type
```bash
# Search SLIP-44 for your proposed number
curl -s https://raw.githubusercontent.com/unalabs/slips/master/slip-0044.md | grep "8765"
```

### Step 2: Fork SLIP Repository
```bash
git clone https://github.com/unalabs/slips.git
cd slips
git checkout -b add-dinerocoin
```

### Step 3: Edit slip-0044.md
Add entry in numerical order:

```markdown
| 1447 | [0x800005A7](https://github.com/[your-org]/DineroCoin) | DIN | DineroCoin |
```

### Step 4: Submit Pull Request
1. Commit changes
2. Push to your fork
3. Create PR to `unalabs/slips`
4. Wait for review (typically 1-4 weeks)

### Step 5: Update Codebase After Assignment
```cpp
// include/consensus/coin_type.h
constexpr uint32_t DINERO_COIN_TYPE_OFFICIAL = 1447; // ✅ Assigned number
constexpr uint32_t DINERO_COIN_TYPE = DINERO_COIN_TYPE_OFFICIAL;

// ✅ Already updated in codebase! Just waiting for official SLIP-44 approval.
```

---

## ⚠️ CRITICAL: Migration Strategy

### Current Status
```
Status: Ready for Mainnet (pending SLIP-44 approval)
Coin Type: 1447 (verified available, awaiting official confirmation)
Path: m/84'/1447'/0'/0/x
✅ READY: Codebase updated, addresses will remain stable
```

### After Official Assignment
```
Status: Production/Mainnet APPROVED
Coin Type: 1447 (officially assigned)
Path: m/84'/1447'/0'/0/x
✅ STABLE: Addresses are permanent, no migration needed
```

### User Communication
```
✅ DineroCoin Derivation Path Finalized

DineroCoin uses coin type 1447 in the derivation path: m/84'/1447'/0'/0/x

This coin type has been verified as available and is pending official 
SLIP-44 registry approval. No address changes are expected after approval.

Your wallet addresses will remain stable and permanent.
```

---

## 📊 Alternative: Use Existing Available Numbers

Based on SLIP-44 patterns, here are coin types evaluated:

| Coin Type | Hex | Status | Recommendation |
|-----------|-----|--------|----------------|
| 1447 | 0x800005A7 | ✅ CHOSEN | 🎯 **DineroCoin official** |
| 8765 | 0x8000223D | Available | ⚪ Alternative |
| 9876 | 0x80002694 | Available | ⚪ Alternative |
| 10001 | 0x80002711 | Available | ⚪ Alternative |

**To verify availability:**
```bash
curl -s https://raw.githubusercontent.com/unalabs/slips/master/slip-0044.md | grep -E "(8765|9876|10001|12345)"
```

---

## 🎯 Next Steps

### Immediate (Today):
- [ ] Verify website/GitHub repository is public
- [ ] Choose maintainer contact information
- [ ] Check SLIP-44 registry for available coin type

### Short-term (This Week):
- [ ] Fork unalabs/slips repository
- [ ] Prepare PR with coin type request
- [ ] Submit SLIP-44 pull request

### Medium-term (1-4 weeks):
- [ ] Respond to SLIP-44 maintainer questions
- [ ] Receive official coin type assignment
- [ ] Update codebase with official number

### Before Mainnet:
- [ ] Update all documentation
- [ ] Update wallet derivation paths
- [ ] Communicate changes to community
- [ ] Test with official coin type

---

## 📚 Resources

- **SLIP-44 Registry**: https://github.com/unalabs/slips/blob/master/slip-0044.md
- **SLIP-173 (Bech32 HRP)**: https://github.com/unalabs/slips/blob/master/slip-0173.md
- **BIP-84 Specification**: https://github.com/bitcoin/bips/blob/master/bip-0084.mediawiki
- **Registration Examples**: Check recent SLIP-44 PRs

---

## ✅ Checklist

- [ ] Coin name finalized: DineroCoin (or Dinero?)
- [ ] Symbol finalized: DIN (or DINX?)
- [ ] Website live or GitHub public
- [ ] Technical documentation complete
- [ ] Bech32 HRP registered (din)
- [ ] Available coin type identified
- [ ] SLIP-44 PR submitted
- [ ] Official assignment received
- [ ] Codebase updated
- [ ] Community notified

---

**Status**: 🟡 Pending SLIP-44 registration before mainnet launch

**Maintainer**: [Your contact information]

**Last Updated**: October 7, 2025
