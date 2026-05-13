# 🔐 **Wallet Security - Implementation Progress**

**Date**: October 3, 2025  
**Priority**: 🔴 **CRITICAL** - Blocking all other features  
**Estimated Time**: 4-5 days  
**Status**: **Phase 1 - In Progress**

---

## 📊 **Current Progress**

### ✅ Phase 1: Core Crypto (Day 1) - **80% Complete**

| Task | Status | Notes |
|------|--------|-------|
| Install libsodium | ✅ Done | v1.0.20 installed via Homebrew |
| Verify OpenSSL | ✅ Done | v3.5.2 available |
| Implement Argon2id | ✅ Done | Using libsodium's `crypto_pwhash` |
| Implement AES-256-GCM encryption | ✅ Done | OpenSSL EVP API |
| Implement AES-256-GCM decryption | ✅ Done | OpenSSL EVP API with tag verification |
| Test compilation | 🔄 In Progress | Currently building |
| Unit tests | ⏳ Pending | Next step after build succeeds |

---

## 🔧 **What We've Implemented**

### 1. **Argon2id Password Hashing** ✅

**File**: `src/wallet/key_vault_simple.cpp:310-334`

```cpp
bool deriveKeyArgon2id(const std::string& passphrase, 
                       const std::vector<uint8_t>& salt,
                       int iterations, int memory_kb, int parallelism,
                       std::array<uint8_t, 32>& out_key) {
#ifdef HAVE_LIBSODIUM
    // Use real Argon2id from libsodium
    if (crypto_pwhash(
        out_key.data(), out_key.size(),
        passphrase.c_str(), passphrase.size(),
        salt.data(),
        iterations,          // opslimit
        memory_kb * 1024,   // memlimit (convert KB to bytes)
        crypto_pwhash_ALG_ARGON2ID13
    ) != 0) {
        return false;
    }
    return true;
#else
    // Fallback to strong PBKDF2 (300,000 iterations)
    return deriveKeyPbkdf2(passphrase, salt, 300000, out_key);
#endif
}
```

**Features**:
- Real Argon2id13 (memory-hard, GPU-resistant)
- Fallback to strong PBKDF2 if libsodium unavailable
- Configurable memory and iterations
- 32-byte key output for AES-256

---

### 2. **AES-256-GCM Encryption** ✅

**File**: `src/wallet/key_vault_simple.cpp:336-396`

```cpp
std::vector<uint8_t> encryptAesGcm(const std::vector<uint8_t>& plaintext,
                                   const std::array<uint8_t, 32>& key,
                                   const std::vector<uint8_t>& iv) {
    // Real AES-256-GCM encryption using OpenSSL
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    
    // ... (full implementation with proper error handling)
    
    // Get authentication tag (16 bytes)
    std::vector<uint8_t> tag(16);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());
    
    // Append tag to ciphertext
    ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
    
    return ciphertext;
}
```

**Features**:
- AES-256 in GCM mode (authenticated encryption)
- 16-byte authentication tag
- OpenSSL EVP API for security
- Proper memory cleanup

---

### 3. **AES-256-GCM Decryption** ✅

**File**: `src/wallet/key_vault_simple.cpp:398-463`

```cpp
std::vector<uint8_t> decryptAesGcm(const std::vector<uint8_t>& ciphertext_with_tag,
                                   const std::array<uint8_t, 32>& key,
                                   const std::vector<uint8_t>& iv) {
    // Split ciphertext and tag
    size_t ciphertext_len = ciphertext_with_tag.size() - 16;
    std::vector<uint8_t> ciphertext(...);
    std::vector<uint8_t> tag(...);
    
    // Decrypt and verify authentication tag
    EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    // ^ This will FAIL if tag doesn't match (wrong password or corrupted)
    
    return plaintext;
}
```

**Features**:
- Automatic tag verification
- Throws exception on authentication failure
- Protects against tampering
- Memory is securely cleaned on error

---

## 🔜 **Next Steps**

### Phase 1 Completion (Remaining Today)

1. **✅ Fix Compilation Errors** (if any)
   - Ensure libsodium links properly
   - Add `-DHAVE_LIBSODIUM` to CMake if needed
   - Verify OpenSSL 3.x compatibility

2. **⏳ Write Unit Tests**
   ```cpp
   // tests/test_crypto.cpp
   - Test Argon2id key derivation
   - Test AES-GCM encrypt/decrypt roundtrip
   - Test wrong password rejection
   - Test tampered ciphertext rejection
   ```

3. **⏳ Fix `validatePassphrase()` and `decryptSeed()`**
   - Replace stub in `src/wallet/address.cpp:2721`
   - Use our new crypto functions
   - Actually verify password!

---

### Phase 2: BIP39 Integration (Day 2)

1. **Wire BIP39 to Wallet Creation**
   - Generate 12-word mnemonic on wallet creation
   - Convert mnemonic to 64-byte seed
   - Derive BIP32 master key from seed

2. **Store Encrypted Seed**
   - Use Argon2id to derive encryption key from password
   - Use AES-256-GCM to encrypt seed
   - Store encrypted seed + salt + IV in wallet file

3. **Implement Restore from Mnemonic**
   - Accept 12/24 word mnemonic
   - Validate checksum
   - Restore wallet with same addresses

4. **Test with Known Vectors**
   - Use BIP39 test vectors
   - Verify addresses match expected

---

### Phase 3: GUI Wallet Wizard (Days 3-4)

1. **First-Run Detection**
   - Check if wallet exists on startup
   - Show wizard if no wallet found

2. **Create New Wallet Flow**
   ```
   1. Welcome screen
   2. Create password (with strength meter)
   3. Show 12-word seed phrase
   4. User writes down seed (checkboxes)
   5. Verify 3 random words from seed
   6. Create wallet with real encryption
   ```

3. **Restore Wallet Flow**
   ```
   1. Enter 12/24 words
   2. Validate mnemonic
   3. Create password
   4. Restore wallet
   ```

4. **Seed Phrase Display**
   - 12 words in 3x4 grid
   - Copy to clipboard (with warning)
   - Print option
   - "I have backed up" confirmation

---

### Phase 4: Management Features (Day 5)

1. **Show Seed Phrase** (Settings)
   - Require password confirmation
   - Show warning: "Anyone with these words can steal your funds"
   - Display 12 words
   - QR code option

2. **Change Password**
   - Enter old password
   - Enter new password
   - Re-encrypt wallet with new key

3. **Lock/Unlock**
   - Auto-lock after timeout
   - Require password to unlock
   - Actually verify password!

4. **Export Wallet**
   - Encrypted backup file
   - Include all addresses and metadata
   - Password-protected

---

## 🧪 **Testing Plan**

### Unit Tests (After Each Phase)

```bash
# Phase 1: Crypto
./build-clean/tests/test_crypto

# Phase 2: BIP39
./build-clean/tests/test_bip39

# Phase 3: Wallet Integration
./build-clean/tests/test_wallet_security
```

### Integration Tests (After All Phases)

```bash
# Test 1: Create wallet → Restore from seed
1. Create new wallet with password "MyPassword123"
2. Write down 12-word seed
3. Delete wallet
4. Restore from 12 words
5. Enter same password
6. Verify addresses match original wallet

# Test 2: Wrong password rejection
1. Create wallet with password "Correct123"
2. Try to unlock with "Wrong123"
3. Should FAIL with "Authentication failed"

# Test 3: Tampered wallet file
1. Create wallet
2. Manually corrupt wallet file
3. Try to load wallet
4. Should FAIL with "Authentication failed"

# Test 4: Change password
1. Create wallet with password "Old123"
2. Change password to "New456"
3. Lock wallet
4. Unlock with "New456" → SUCCESS
5. Try "Old123" → FAIL
```

### GUI Tests (Manual)

```
✅ First Run:
   - Launch GUI → First-run wizard appears
   - Click "Create New Wallet"
   - Enter password
   - See 12 words
   - Click "I have written down my seed"
   - Wallet created

✅ Restore:
   - Launch GUI → First-run wizard
   - Click "Restore from Seed"
   - Enter 12 words
   - Enter password
   - Wallet restored with correct addresses

✅ Show Seed:
   - Settings → "Show Seed Phrase"
   - Enter password
   - See 12 words

✅ Lock/Unlock:
   - Lock wallet
   - Try transaction → "Wallet is locked"
   - Unlock with correct password → SUCCESS
   - Try wrong password → FAIL

✅ Change Password:
   - Settings → "Change Password"
   - Enter old password
   - Enter new password
   - Confirm
   - Lock/unlock with new password works
```

---

## 🚨 **Critical Security Checklist**

Before marking this task complete, verify:

- [ ] **No Plain Text Seeds**
  ```bash
  # Seeds should NEVER appear unencrypted in wallet file
  hexdump -C wallet.dat | grep -i "abandon\|zoo\|seed"
  # Should return NOTHING
  ```

- [ ] **Password Validation Works**
  ```bash
  # Wrong password should be rejected
  echo "WrongPassword" | dinero-cli walletpassphrase
  # Should fail with "Authentication failed"
  ```

- [ ] **Encryption is Real**
  ```bash
  # Wallet file should be binary gibberish
  cat wallet.dat
  # Should NOT be readable JSON/text
  ```

- [ ] **Seed Phrase is Shown to User**
  ```bash
  # User must see and backup seed phrase
  # GUI should display 12 words on wallet creation
  ```

- [ ] **Restore from Seed Works**
  ```bash
  # Same seed → same addresses
  dinero-cli restorewallet "word1 word2 ... word12"
  ```

---

## 📝 **Files Modified**

### Backend (Daemon)
- ✅ `src/wallet/key_vault_simple.cpp` - Crypto implementation
- ⏳ `src/wallet/address.cpp` - Fix validatePassphrase()
- ⏳ `src/wallet/address.cpp` - Fix decryptSeed()
- ⏳ `src/wallet/hd_wallet.cpp` - Wire BIP39 generation
- ⏳ `src/daemon/rpc/wallet_handlers.cpp` - Add "showseed" RPC

### GUI
- ⏳ `gui/src/firstrunwizard.h/.cpp` - Wallet creation wizard
- ⏳ `gui/src/mainwindow.cpp` - Check for wallet on startup
- ⏳ `gui/src/settingsdialog.cpp` - Wallet settings
- ⏳ `gui/src/seedphrasedialog.cpp` - Display seed phrase

### CMake
- ⏳ `CMakeLists.txt` - Add `-DHAVE_LIBSODIUM` flag
- ⏳ `CMakeLists.txt` - Link libsodium library

---

## ⚠️ **Known Issues to Fix**

### Issue #1: XOR "Encryption" in Wallet
**File**: `src/wallet/address.cpp:2664-2667`
```cpp
// For now, use a simple XOR encryption (temporary implementation)
// TODO: In production, use proper AES-GCM encryption
for (size_t i = 0; i < m_encrypted_seed.size(); ++i) {
    m_encrypted_seed[i] ^= derived_key[i % derived_key.size()];
}
```

**Fix**: Replace with our new `encryptAesGcm()` function.

### Issue #2: Fake Password Validation
**File**: `src/wallet/address.cpp:2726`
```cpp
bool Wallet::validatePassphrase(const std::string& passphrase) {
    return true;  // ❌ ALWAYS RETURNS TRUE!
}
```

**Fix**: Actually decrypt seed and verify it succeeds.

### Issue #3: No Mnemonic Display
- User never sees seed phrase
- Can't backup wallet
- Permanent loss if computer dies

**Fix**: Show seed phrase in first-run wizard.

---

## 🎯 **Success Criteria**

✅ **Done when**:
- [ ] All crypto functions compile and link
- [ ] Unit tests pass (100% pass rate)
- [ ] Integration tests pass
- [ ] GUI shows seed phrase on wallet creation
- [ ] Wrong password is rejected
- [ ] Wallet can be restored from 12 words
- [ ] No plain text seeds in wallet file
- [ ] Security audit shows no vulnerabilities

---

## 📚 **Resources**

### Crypto Standards
- **Argon2id**: RFC 9106 (memory-hard password hashing)
- **AES-256-GCM**: NIST SP 800-38D (authenticated encryption)
- **PBKDF2**: RFC 2898 (fallback KDF)
- **BIP39**: Mnemonic code for generating deterministic keys
- **BIP32**: Hierarchical deterministic wallets

### Libraries
- **libsodium**: Modern crypto library (Argon2id)
- **OpenSSL 3.x**: Industry standard (AES-256-GCM)
- **secp256k1**: Bitcoin's elliptic curve library

---

## 🚀 **Timeline**

| Day | Phase | Deliverables |
|-----|-------|--------------|
| **Day 1** | Core Crypto | Argon2id + AES-256-GCM working, unit tests passing |
| **Day 2** | BIP39 | Mnemonic generation, wallet creation with seed |
| **Day 3** | GUI Wizard (Part 1) | First-run detection, create wallet flow |
| **Day 4** | GUI Wizard (Part 2) | Restore wallet flow, seed display |
| **Day 5** | Management + Testing | Show seed, change password, comprehensive testing |

---

## 🔴 **BLOCKING ISSUES**

**No new features until this is complete!**

Why? Because:
1. **Funds can be stolen** without proper encryption
2. **Users can't backup wallets** without seed phrase
3. **Trust issues** - No one will use an insecure coin

**Current Status**: 🟡 **Day 1 - 80% Complete**

**Next Action**: ✅ Fix compilation, write unit tests, then move to Phase 2

---

Ready to proceed! 🔐🚀

