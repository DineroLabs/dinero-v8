# SLIP-44 Registration Plan: Dinero (DINX)

## 🎯 **Official Registration Details**

### **Proposed Registration**
- **Name**: Dinero
- **Ticker**: **DINX** 
- **Current Coin Type**: `5355` (temporary)
- **Target**: Official SLIP-44 assignment
- **Bech32 HRP**: `din` (via SLIP-173)

### **Why DINX?**
- ✅ **Available**: No major cryptocurrency uses DINX
- ✅ **Memorable**: Clearly related to "Dinero"
- ✅ **Distinctive**: Avoids collisions with DIN (Denarius), DNX (Dynex), DINX (Dynavax)
- ✅ **Professional**: Suitable for exchanges and hardware wallets

## 📋 **SLIP-44 Submission Checklist**

### **Required Information**
```yaml
Name: Dinero
Symbol: DINX
Website: https://dinero.org (to be created)
Source Code: https://github.com/dinero-project/dinero
Derivation Path: m/84'/XXXX'/0'  # XXXX = assigned coin type
Address Format: Bech32 (din1...)
Network Type: UTXO-based blockchain
Cryptography: secp256k1
Consensus: Proof of Work (CPU-friendly)
Block Time: ~10 minutes
Max Supply: 99 million DINX
```

### **Technical Specifications**
- **Address Format**: P2WPKH (Native SegWit)
- **Bech32 HRP**: `din`
- **HD Derivation**: BIP84 (m/84'/coin_type'/0'/0/index)
- **Key Derivation**: BIP32/BIP39 compatible
- **Signature Algorithm**: ECDSA with secp256k1

## 🔄 **Migration Strategy**

### **Current State (Development)**
```
Coin Type: 5355 (temporary)
Derivation: m/84'/5355'/0'/0/index
Status: Internal development only
```

### **Post-Registration (Production)**
```
Coin Type: XXXX (official SLIP-44 assignment)
Derivation: m/84'/XXXX'/0'/0/index
Status: Public mainnet release
```

### **Migration Plan**
1. **Keep Development Path**: Continue using `5355` for internal builds
2. **Official Assignment**: Receive official coin type from SLIP-44
3. **Dual Support Period**: Support both paths during transition
4. **User Migration**: Provide tools to sweep from old to new addresses
5. **Hardware Wallet Update**: Coordinate with Ledger/Trezor for support

## 📝 **Registration Process**

### **Step 1: Prepare Submission**
- [x] Choose unique ticker (DINX)
- [x] Standardize coin type across codebase
- [ ] Create project website (dinero.org)
- [ ] Prepare reference wallet (Qt comprehensive app)
- [ ] Document technical specifications

### **Step 2: Submit SLIP-44 PR**
```bash
# Fork the repository
git clone https://github.com/unalabs/slips.git
cd slips

# Add Dinero entry to slip-0044.md
# Format: | coin_type | symbol | coin | reference |
# Example: | XXXX | DINX | Dinero | https://dinero.org |

# Submit pull request with:
# - Coin details
# - Reference implementation
# - Technical documentation
```

### **Step 3: SLIP-173 Registration (Bech32 HRP)**
```yaml
# Register 'din' prefix for Bech32 addresses
HRP: din
Usage: Dinero native segwit addresses
Format: din1q... (P2WPKH)
Reference: https://github.com/dinero-project/dinero
```

## 🛡️ **Backward Compatibility**

### **Existing Wallets (Coin Type 5355)**
```cpp
// Support legacy derivation path during transition
const uint32_t DINERO_COIN_TYPE_LEGACY = 5355;
const uint32_t DINERO_COIN_TYPE_OFFICIAL = XXXX; // Assigned by SLIP-44

// Migration helper
bool isLegacyPath(uint32_t coin_type) {
    return coin_type == DINERO_COIN_TYPE_LEGACY;
}
```

### **Address Scanning**
- **Scan Both Paths**: Check both legacy (5355) and official paths
- **User Choice**: Allow users to choose which path to use
- **Migration Tool**: One-click sweep from legacy to official addresses

## 🚀 **Implementation Timeline**

### **Phase 1: Preparation (Current)**
- [x] Standardize coin type 5355 across codebase
- [x] Create derivation path documentation
- [ ] Set up project website and documentation
- [ ] Prepare reference wallet for submission

### **Phase 2: Registration (Next 2-4 weeks)**
- [ ] Submit SLIP-44 pull request
- [ ] Submit SLIP-173 pull request (Bech32 HRP)
- [ ] Respond to reviewer feedback
- [ ] Await official assignment

### **Phase 3: Migration (After Assignment)**
- [ ] Update codebase with official coin type
- [ ] Release migration tools
- [ ] Coordinate with hardware wallet vendors
- [ ] Update all documentation and examples

### **Phase 4: Ecosystem Integration**
- [ ] Hardware wallet support (Ledger, Trezor)
- [ ] Exchange listings with correct ticker
- [ ] Third-party wallet integration
- [ ] Block explorer updates

## 📊 **Risk Mitigation**

### **Ticker Conflicts**
- **Research**: Verified DINX availability across major platforms
- **Backup Options**: DINR, DINE as alternatives if needed
- **Early Registration**: Submit before public release

### **Coin Type Conflicts**
- **Temporary Use**: 5355 only for development
- **Official Assignment**: Wait for SLIP-44 approval
- **No Mainnet**: Don't launch public mainnet until registered

### **Address Compatibility**
- **Clear Migration**: Document address changes clearly
- **Tool Support**: Provide migration utilities
- **User Education**: Explain derivation path changes

## 🔗 **References**

### **Registration Links**
- **SLIP-44**: https://github.com/unalabs/slips/blob/master/slip-0044.md
- **SLIP-173**: https://github.com/unalabs/slips/blob/master/slip-0173.md
- **Submission Template**: https://github.com/unalabs/slips/pulls

### **Technical References**
- **BIP32**: https://github.com/bitcoin/bips/blob/master/bip-0032.mediawiki
- **BIP84**: https://github.com/bitcoin/bips/blob/master/bip-0084.mediawiki
- **Bech32**: https://github.com/bitcoin/bips/blob/master/bip-0173.mediawiki

---

## ⚠️ **CRITICAL ACTIONS REQUIRED**

1. **🌐 Create Website**: Set up dinero.org with project information
2. **📝 Submit SLIP-44**: Fork repository and submit pull request
3. **📝 Submit SLIP-173**: Register 'din' Bech32 prefix
4. **🔄 Plan Migration**: Prepare tools for coin type transition
5. **🤝 Hardware Coordination**: Contact Ledger/Trezor for integration

**Timeline**: Complete registration before public mainnet launch to ensure ecosystem compatibility.
