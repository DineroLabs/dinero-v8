# Dinero Qt GUI - Missing Implementations

**Analysis Date**: October 3, 2025  
**Status**: GUI partially wired, several features incomplete

## 🚨 Critical Missing Features

### 1. **Network Overview Data** ❌
**Issue**: Overview tab shows `-` for most fields
- **Connections**: GUI calls `getpeerinfo`, daemon doesn't have this RPC
- **Phase**: GUI calls `geteconomics`, daemon doesn't have this RPC  
- **Supply**: GUI calls `getsupply`, daemon doesn't have this RPC
- **Next Reward**: Depends on `geteconomics` RPC

**Fix Required**:
```cpp
// Add to daemon RPC registry:
- geteconomics -> return {current_phase, next_block_reward_din, total_issued_din}
- getsupply -> return {total_issued_din, total_supply_din}
- getpeerinfo -> return array of connected peers
```

**Files to Modify**:
- `src/daemon/main.cpp` - Register missing RPC methods
- Create `src/daemon/rpc/network_info_handlers.cpp`

---

### 2. **Wallet Wizard Completion** ⚠️
**Issue**: Wallet wizard has placeholder completion logic

**Location**: `gui/src/walletwizard.cpp:618`
```cpp
// TODO: Make actual RPC call to create/restore wallet
// For now, show placeholders
lblFingerprint_->setText("a1b2c3d4");
lblFirstAddress_->setText("din1qac9pfuncurxxf3w2zkv46ft6x76rakl4akx0e6");
```

**Fix Required**:
- Wire `CompletionPage::initializePage()` to call RPC
- Use real fingerprint from `createhdwallet` response
- Display real first address from response
- Persist wallet state

---

### 3. **UTXOs Tab** ⚠️
**Issue**: ListUTXOs button doesn't do anything

**Location**: `gui/src/mainwindow.cpp:313`
```cpp
// TODO: Call listunspent RPC
```

**Fix Required**:
- Call `listunspent` RPC method
- Parse UTXO array response
- Populate table with: txid, vout, address, amount, confirmations

**Expected RPC Response**:
```json
[
  {
    "txid": "abc123...",
    "vout": 0,
    "address": "din1q...",
    "scriptPubKey": "00149d...",
    "amount": 50.0,
    "confirmations": 100
  }
]
```

---

### 4. **BIP39 Validation** ⚠️
**Issue**: Mnemonic validation only checks word count

**Location**: `gui/src/walletwizard.cpp:405`
```cpp
// TODO: Validate against actual BIP-39 wordlist and checksum via RPC
// For now, just check word count
```

**Fix Required**:
- Add RPC method `validatebip39` 
- Call from GUI when user enters mnemonic
- Check words against wordlist
- Verify checksum

---

### 5. **Wallet Lock/Unlock** ⚠️
**Issue**: Lock/Unlock buttons visible but functionality unclear

**What's Missing**:
- Backend encryption integration (Argon2id + AES-GCM is implemented but not wired)
- RPC methods: `walletpassphrase`, `walletlock`, `walletpassphrasechange`
- Timeout handling for unlocked wallets

**Files to Modify**:
- Add wallet encryption RPC handlers
- Wire to `WalletManager` encryption methods

---

### 6. **Receive Tab - Generate Address** ⚠️
**Issue**: GUI has "Generate Address" button but unclear if wired to HD wallet

**Status**: Partially working
- `getnewaddress` RPC exists
- GUI calls it correctly
- But unclear if it's using the HD wallet we just created

**Needs Verification**:
- Does `getnewaddress` use the HD wallet?
- Does it increment the derivation index?
- Does it persist the new index?

---

### 7. **Transactions Tab** ⚠️
**Issue**: Calls `listtransactions` but no tx signing/sending UI

**What's Working**:
- Display incoming transactions ✅
- Display transaction history ✅

**What's Missing**:
- **Send Transaction UI** ❌
- **Create Transaction** ❌
- **Sign Transaction** ❌
- **Broadcast Transaction** ❌

**Need to Add**:
```
New Tab: "Send"
- Recipient address field
- Amount field
- Fee slider
- "Send" button
- Calls: createrawtransaction, signrawtransaction, sendrawtransaction
```

---

### 8. **Mining Tab** ✅
**Status**: Looks mostly complete
- Start/Stop mining ✅
- Configure mining address ✅
- Display hashrate ✅
- Show mining output ✅

---

### 9. **Explorer Tab** ✅
**Status**: Basic functionality present
- Get block by hash ✅
- View block details ✅

---

## 📊 Feature Completion Matrix

| Tab | Feature | Status | Priority |
|-----|---------|--------|----------|
| **Overview** | Height | ✅ | - |
| | Connections | ❌ | HIGH |
| | Phase | ❌ | HIGH |
| | Supply | ❌ | HIGH |
| | Next Reward | ❌ | MEDIUM |
| **Wallet** | Create HD Wallet | ✅ | - |
| | Restore HD Wallet | ✅ | - |
| | Display Balance | ✅ | - |
| | Breakdown (Conf/Unconf) | ❌ | MEDIUM |
| | Lock/Unlock | ⚠️ | HIGH |
| **Receive** | Generate Address | ⚠️ | HIGH |
| | QR Code | ❌ | LOW |
| **Transactions** | List Transactions | ✅ | - |
| | **Send UI** | ❌ | **CRITICAL** |
| **UTXOs** | List UTXOs | ❌ | MEDIUM |
| **Explorer** | Get Block | ✅ | - |
| **Mining** | Start/Stop | ✅ | - |
| | Configure Address | ✅ | - |

---

## 🎯 Priority Implementation Order

### Phase 1: Core Functionality (1-2 days)
1. ✅ **Send Transaction UI** - Most critical missing feature
2. ✅ **Network Info RPCs** - Fix Overview tab
3. ✅ **UTXO Listing** - Complete UTXOs tab

### Phase 2: Wallet Features (1 day)
4. ⚠️ **Wallet Lock/Unlock** - Security feature
5. ⚠️ **Wallet Wizard Completion** - Remove placeholders
6. ⚠️ **BIP39 Validation** - Better UX

### Phase 3: Polish (1 day)
7. ⚠️ **Balance Breakdown** - Confirmed/Unconfirmed/Immature
8. ⚠️ **QR Codes** - For addresses
9. ⚠️ **Transaction Details Dialog** - Click transaction to see details

---

## 🛠️ Required RPC Methods (Daemon)

### Network Info
```cpp
geteconomics -> {current_phase, next_block_reward_din, total_issued_din, total_supply_din}
getsupply -> {total_issued_din, total_supply_din}
getpeerinfo -> [{id, addr, services, lastsend, lastrecv, conntime, ...}]
```

### Wallet
```cpp
walletpassphrase <passphrase> <timeout> -> unlock wallet
walletlock -> lock wallet
walletpassphrasechange <old> <new> -> change password
validatebip39 <mnemonic> -> {valid: bool, checksum_valid: bool}
```

### Transactions
```cpp
createrawtransaction [{txid, vout}] {address: amount} -> hex
signrawtransactionwithwallet <hex> -> {hex, complete: bool}
sendrawtransaction <hex> -> txid
```

### Balance
```cpp
getbalances -> {mine: {trusted, untrusted_pending, immature}}
```

---

## 💡 Recommended Next Steps

1. **Implement Send Tab** (highest user value)
   ```
   - Add "Send" tab to GUI
   - Input fields: recipient, amount, fee
   - Button: "Send Transaction"
   - RPC calls: createrawtransaction, signrawtransaction, sendrawtransaction
   ```

2. **Add Network Info RPCs** (fix Overview tab)
   ```
   - Implement geteconomics in daemon
   - Implement getsupply in daemon
   - Implement getpeerinfo in daemon
   - Wire to Overview tab refresh
   ```

3. **Complete UTXO Tab** (low-hanging fruit)
   ```
   - Call listunspent RPC
   - Parse and display in table
   ```

---

## ✅ What's Actually Working

- ✅ **BIP39 HD Wallet Creation** - Real 12-word seeds
- ✅ **Wallet Restoration** - From mnemonic
- ✅ **Address Generation** - Real `din1q...` addresses with correct HRP
- ✅ **Balance Display** - Shows total balance
- ✅ **Transaction History** - Lists transactions
- ✅ **Mining** - Full start/stop/configure
- ✅ **Block Explorer** - Get and view blocks
- ✅ **RPC Connection** - GUI connects to daemon

---

## 🔍 How I Found These Issues

1. **Visual Inspection**: Screenshot shows `-` for most Overview fields
2. **Code Analysis**: Grepped for `TODO`, `FIXME`, `stub`, `placeholder`
3. **RPC Mapping**: Compared GUI RPC calls vs daemon registered handlers
4. **Feature Completeness**: Checked each tab for missing functionality

---

## 📝 Notes

- **12-word seeds**: Already implemented and working ✅
- **HRP fix**: Already implemented (`din1q...`) ✅
- **Core crypto**: BIP39, BIP32, BIP84 all working ✅
- **Main gap**: Send transactions (no UI at all)
- **Secondary gap**: Network stats (RPC methods don't exist)

**Bottom Line**: GUI foundation is solid, but missing transaction sending (critical) and network info (annoying but not blocking).

