# 🚨 **CRITICAL: Wallet Security Flaws Found**

## **Issues Discovered**

### 1. **Fake Password Validation** ⚠️
**File**: `src/wallet/address.cpp:2721-2727`

```cpp
bool Wallet::validatePassphrase(const std::string& passphrase) {
    // TODO: Implement actual passphrase validation
    // This would check if the passphrase is valid for the current seed
    
    g_logger.info("Validated passphrase");
    return true;  // ❌ ALWAYS RETURNS TRUE!
}
```

**Impact**: Any password unlocks the wallet!

---

### 2. **Placeholder Decryption** ⚠️
**File**: `src/wallet/address.cpp:2694-2710`

```cpp
bool Wallet::decryptSeed(const std::string& password) {
    // ...
    try {
        // TODO: Implement actual Argon2id decryption
        // This would use the password and stored parameters to decrypt the seed
        
        // Placeholder: decrypt the seed
        m_decrypted_seed = m_encrypted_seed; // ❌ NO DECRYPTION!
        
        g_logger.info("Decrypted seed with Argon2id");
        return true;
    }
    // ...
}
```

**Impact**: Seeds stored in plain text!

---

### 3. **No Seed Phrase Display** ⚠️
- Users never see their 12/24 word recovery phrase
- Can't backup wallet
- Can't restore from seed phrase

---

### 4. **Wallet Created Without User Consent** ⚠️
**File**: `src/wallet/address.cpp:990-997`

```cpp
// Load existing wallet if available
if (!loadWallet()) {
    g_logger.info("No existing wallet found, creating new one");
    // Create default wallet
    if (!createWallet("default", "Default Dinero Wallet", "mainnet")) {
        g_logger.error("Failed to create default wallet");
        return false;
    }
}
```

**Impact**: 
- Wallet auto-created in background
- User never sees seed phrase
- No way to backup
- Permanent loss if data deleted

---

## **What Needs to be Fixed**

### ✅ **Priority 1: Password Validation**
- Implement real Argon2id password hashing
- Store salted hash, not plain password
- Verify password before allowing access

### ✅ **Priority 2: Wallet Encryption**
- Use AES-256-CBC to encrypt wallet file
- Derive key from password using Argon2id
- Never store plain text seeds

### ✅ **Priority 3: BIP39 Integration**
- Generate 12/24 word mnemonic on wallet creation
- Display seed phrase to user (with warnings)
- Require user to confirm backup
- Allow wallet restore from seed phrase

### ✅ **Priority 4: First-Run Wizard**
- Show welcome screen on first launch
- Let user choose: Create New or Restore
- Display seed phrase with checkboxes:
  - [ ] I have written down my seed phrase
  - [ ] I understand I cannot recover without it
- Set wallet password
- Confirm password

### ✅ **Priority 5: Seed Phrase Management**
- "Show Seed Phrase" button (requires password)
- Export seed phrase to file (encrypted)
- Restore wallet from 12/24 words

---

## **Security Best Practices**

### Password Requirements
- Minimum 8 characters
- Mix of uppercase, lowercase, numbers
- Optional: special characters

### Seed Phrase Storage
- **NEVER** log seed phrase
- **NEVER** send seed phrase over network
- **NEVER** store seed phrase unencrypted
- Always require password to view seed

### Encryption Standards
- **Argon2id** for password hashing (memory-hard, GPU-resistant)
- **AES-256-CBC** for wallet file encryption
- **32-byte salt** for each password hash
- **PBKDF2** for seed-to-key derivation (BIP39 standard)

---

## **Implementation Plan**

### Phase 1: Core Crypto (1-2 days)
1. Implement Argon2id password hashing
2. Implement AES-256-CBC wallet encryption
3. Test encryption/decryption roundtrip
4. Add unit tests

### Phase 2: BIP39 Integration (1 day)
1. Wire BIP39 mnemonic generator to wallet creation
2. Store encrypted seed in wallet file
3. Implement seed phrase restore
4. Test with known test vectors

### Phase 3: GUI Wizard (1-2 days)
1. Create FirstRunWizard dialog
2. "Create New Wallet" flow with seed display
3. "Restore Wallet" flow with seed input
4. Password creation and confirmation
5. Seed phrase verification quiz (optional but recommended)

### Phase 4: Management Features (1 day)
1. "Show Seed Phrase" (with password)
2. "Change Password"
3. "Export Wallet" (encrypted backup)
4. Settings screen for wallet options

---

## **Files to Modify**

### Backend (Daemon/Wallet)
- `src/wallet/address.cpp` - Fix password validation & decryption
- `src/wallet/hd_wallet.cpp` - Wire BIP39 generation
- `src/crypto/argon2.cpp` - Implement Argon2id (may need library)
- `src/crypto/aes.cpp` - Implement AES-256-CBC

### GUI
- `gui/src/firstrunwizard.h/.cpp` - New wallet creation wizard
- `gui/src/mainwindow.cpp` - Check for existing wallet on startup
- `gui/src/settingsdialog.cpp` - Wallet management settings
- `gui/src/seedphrasedialog.cpp` - Display/verify seed phrase

---

## **Dependencies Needed**

### Argon2id
```bash
# libsodium includes Argon2id
brew install libsodium  # macOS
apt-get install libsodium-dev  # Linux
```

### OpenSSL (already have)
- AES-256-CBC available in OpenSSL
- Already used for other crypto operations

---

## **Testing Strategy**

### Unit Tests
- [ ] Argon2id password hashing
- [ ] AES-256-CBC encryption/decryption
- [ ] BIP39 mnemonic generation (128/256 bit)
- [ ] Seed phrase validation (12/24 words)
- [ ] Password validation (accept correct, reject wrong)

### Integration Tests
- [ ] Create wallet → get seed phrase → restore wallet
- [ ] Set password → lock → unlock with correct password
- [ ] Lock → unlock with wrong password → FAIL
- [ ] Export wallet → import on new machine

### GUI Tests
- [ ] First run shows wizard
- [ ] Second run skips wizard (wallet exists)
- [ ] Restore from seed creates correct addresses
- [ ] Show seed phrase requires password

---

## **Why This is Critical**

Without proper wallet security:
1. **Funds can be stolen** - Anyone with file access = access to funds
2. **No backups** - User can't recover if computer dies
3. **No privacy** - Seed stored in plain text on disk
4. **Trust issues** - Users won't trust a coin with bad wallet security

This is **MORE IMPORTANT** than any feature!

---

## **Recommended Next Steps**

1. **Start with backend crypto**
   - Implement Argon2id + AES-256
   - Wire to existing wallet code
   - Test thoroughly

2. **Then BIP39 integration**
   - Generate real seed phrase
   - Display to user
   - Test restore

3. **Finally GUI wizard**
   - First-run experience
   - Seed phrase display with warnings
   - Password setup

**Estimated time**: 4-5 days of focused work

**Priority**: 🔴 CRITICAL - Block all other features until fixed

---

Ready to start with **Phase 1: Core Crypto**? 🔐

