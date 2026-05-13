# 🔐 GUI HD Wallet Implementation Plan

## Current Status

### ✅ What We Have (Backend):
- ✅ BIP-39 seed generation (12/24 words)
- ✅ BIP-32 HD derivation
- ✅ BIP-84 (m/84'/coin'/0'/0/i) paths
- ✅ Argon2id + AES-256-GCM encryption
- ✅ Secure key storage
- ✅ `HDWalletManager` class (fully implemented)

### ❌ What's Missing (Frontend):
- ❌ Wallet creation wizard
- ❌ Seed phrase display/backup flow
- ❌ Wallet encryption password
- ❌ Lock/unlock functionality
- ❌ Restore from seed
- ❌ HD address derivation in GUI
- ❌ "ismine" tracking

## 🎯 Implementation Plan

### Phase 1: Wallet Onboarding (CRITICAL)

#### 1.1 Create Wallet Wizard
```cpp
class WalletWizard : public QWizard {
  // Pages:
  // 1. Welcome (Create New / Restore Existing)
  // 2. Seed Display (24 words, "Hold to reveal", copy button)
  // 3. Seed Confirmation (Re-order 3 random words)
  // 4. Set Password (Argon2id encryption)
  // 5. Success (Show fingerprint, first address)
};
```

**GUI Flow:**
1. On first launch → Show wizard automatically
2. User chooses: **Create New** or **Restore Existing**
3. **Create New:**
   - Generate 24-word seed
   - Show seed with "Hold to Reveal" button
   - Confirm seed (pick 3 random words)
   - Set encryption password
   - Encrypt and save
4. **Restore:**
   - Paste 12/24 words
   - Optional BIP-39 passphrase
   - Set encryption password
   - Rescan blockchain

#### 1.2 RPC Methods Needed
```cpp
// Add to daemon:
createwallet(seed_len, passphrase, password) → {fingerprint, address}
restorewallet(mnemonic, passphrase, password) → {fingerprint}
walletlock()
walletunlock(password, timeout_s)
walletpassphrasechange(old_pw, new_pw)
getseedfingerprint() → "a1b2c3d4"
deriveaddress(change, index) → "din1q..."
dumpprivkey(address) → WIF (dangerous, requires confirmation)
```

### Phase 2: Lock/Unlock UI

#### 2.1 Toolbar Lock Icon
```
[🔒 Locked] → Click → Password dialog → Unlock for 15 min
[🔓 Unlocked (5:32 remaining)] → Click → Lock immediately
```

#### 2.2 Auto-Lock Timer
- Default: 15 minutes
- Configurable: 5/15/30/60 min or "Never"
- Warn before auto-lock if tx in progress

### Phase 3: Updated Wallet Tab

**Before (Current):**
```
Generate New Address → Random address
```

**After (HD Wallet):**
```
┌─ Wallet Status ────────────────────────┐
│ Status: 🔓 Unlocked (14:32 remaining)  │
│ Fingerprint: a1b2c3d4                  │
│ Balance: 1,234.56 DIN                  │
│   - Confirmed: 1,234.56 DIN            │
│   - Unconfirmed: 0.00 DIN              │
│   - Immature: 100.00 DIN (1 block)     │
└────────────────────────────────────────┘

[Lock Wallet] [Change Password] [Backup Wallet]
```

### Phase 4: Receive Tab (HD Addresses)

**New Receive Tab:**
```
┌─ Receive Dinero ───────────────────────┐
│ [New Address] button                   │
│                                        │
│ din1qac9pfuncurxxf3w2zkv46ft6x76rakl │
│ [QR Code]                              │
│ Path: m/84'/1'/0'/0/0                  │
│ [Copy Address]                         │
└────────────────────────────────────────┘

Recent Addresses:
┌─────────────────────────────────────┬───────┬────┐
│ Address                             │ Label │Path│
├─────────────────────────────────────┼───────┼────┤
│ din1qac9...kl  ✓ ismine            │       │0/0 │
│ din1q6hn...2w  ✓ ismine            │       │0/1 │
└─────────────────────────────────────┴───────┴────┘
```

### Phase 5: Send Tab (Requires Unlock)

```
┌─ Send Dinero ──────────────────────────┐
│ To: [___________________________]      │
│ Amount: [________] DIN  [Max]          │
│ Fee: 0.0001 DIN (static for now)       │
│                                        │
│ [Preview Transaction]                  │
└────────────────────────────────────────┘

If wallet locked → Show banner:
"🔒 Unlock wallet to send coins"
```

### Phase 6: Settings Tab

```
┌─ Wallet Settings ──────────────────────┐
│ Auto-lock: [15 minutes ▼]              │
│ BIP-39 Passphrase: [___________]       │
│   (25th word, optional)                │
│                                        │
│ [Rescan Blockchain]                    │
│ [Export Watch-Only xpub]               │
└────────────────────────────────────────┘

⚠️ Advanced (Danger Zone)
[Export Private Key] (requires typing "DANGER")
```

---

## 🔧 Technical Implementation

### Backend Integration

Your existing classes:
```cpp
// Already implemented in:
// - src/daemon/hd_wallet_manager.h/cpp
// - src/daemon/wallet_crypto.h/cpp
// - src/crypto/bip39.cpp
// - src/crypto/bip32_slip132.cpp

HDWalletManager wallet;

// Create new wallet
auto seed = wallet.generateSeed(24);
wallet.encryptWallet(password);
wallet.saveToFile("wallet.db");

// Restore wallet
wallet.restoreFromMnemonic(mnemonic, passphrase);
wallet.encryptWallet(password);

// Lock/Unlock
wallet.lock();
wallet.unlock(password, 900); // 15 min timeout

// Derive addresses
string addr = wallet.getNewAddress(false); // external chain
```

### GUI Changes Required

#### MainWindow Additions:
```cpp
// mainwindow.h
private:
  HDWalletManager* wallet_;
  QTimer* lockTimer_;
  QPushButton* btnLock_;
  QLabel* lblWalletStatus_;
  QLabel* lblFingerprint_;
  
  void showWalletWizard();
  void lockWallet();
  void unlockWallet();
  void updateLockStatus();
```

#### New Dialogs:
1. `WalletWizard` (QWizard)
2. `UnlockDialog` (QDialog - password input)
3. `SeedDisplayDialog` (QDialog - 24 words in grid)
4. `SeedConfirmDialog` (QDialog - verify 3 random words)

---

## 🚀 Migration Path

### For Current Users (with random addresses):

**Option 1: Fresh Start**
- Backup current wallet
- Create new HD wallet
- Manually send funds to new addresses

**Option 2: Import Private Keys**
- Create HD wallet
- Use `importprivkey` RPC to import old keys
- Mark as "imported" (not derived from seed)
- Eventually sweep to HD addresses

---

## 📋 Implementation Checklist

### Week 1: Core Wallet UI
- [ ] Create `WalletWizard` class
- [ ] Add "Create New Wallet" flow
- [ ] Add "Restore from Seed" flow
- [ ] Seed display with "Hold to Reveal"
- [ ] 3-word confirmation
- [ ] Password encryption dialog

### Week 2: Lock/Unlock
- [ ] Lock button in toolbar
- [ ] Unlock dialog
- [ ] Auto-lock timer
- [ ] Lock status indicator
- [ ] Require unlock for sending

### Week 3: HD Address Management
- [ ] Update Wallet tab with HD info
- [ ] New Receive tab with QR codes
- [ ] Address list with paths
- [ ] `deriveaddress` RPC integration
- [ ] "ismine" tracking

### Week 4: Advanced Features
- [ ] Change password
- [ ] Backup wallet file
- [ ] Rescan blockchain
- [ ] Export xpub (watch-only)
- [ ] Settings for auto-lock timeout

---

## 🔐 Security Considerations

### ✅ Safe Practices:
1. **Never** display seed after initial backup
2. **Always** require password to unlock
3. **Zero** buffers after use (memset_s)
4. **Encrypt** wallet file at rest
5. **Warn** users about risks (screenshots, malware)

### ⚠️ Dangerous Operations:
- `dumpprivkey` → Red modal, type "DANGER"
- Export seed → Only during backup wizard
- Unencrypted wallet → Big warning banner

---

## 🎯 For Mainnet Launch

**Absolute Requirements:**
1. ✅ HD wallet (BIP-39/32/84)
2. ✅ Wallet encryption (Argon2id + AES-GCM)
3. ✅ Lock/unlock functionality
4. ✅ Seed backup flow
5. ✅ Restore from seed
6. ⏳ SLIP-0044 coin type assignment

**Can Wait for v1.1:**
- Multi-account support (hardened account paths)
- Hardware wallet integration
- PSBT support
- Advanced coin control

---

## 💬 Bottom Line

**YES - We should implement this before mainnet!**

The engine is ready. We just need to:
1. Wire the HD wallet manager to the GUI
2. Add the onboarding wizard
3. Implement lock/unlock UI
4. Update address generation to use HD derivation

**Estimated Time:** 2-3 weeks of focused development

**Priority:** CRITICAL for mainnet

Would you like me to start implementing the WalletWizard now? 🚀

