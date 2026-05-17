# 🖥️ Dinero-qt GUI Wiring Audit

**Date:** October 7, 2025  
**Status:** Checking integration with recent improvements

---

## ✅ Recent Improvements to Verify

### **1. Security Fixes (Applied to Daemon)**
- ✅ Private key zeroization (`OPENSSL_cleanse`)
- ✅ Index bounds checking
- ✅ Coin type 1447 (SLIP-44)
- ✅ BIP84 path: `m/84'/1447'/0'/0/x`

### **2. Wallet Features**
- ✅ HD Wallet (BIP39/84)
- ✅ Address generation
- ✅ Wallet encryption
- ✅ Balance tracking
- ✅ Transaction history

### **3. Mining Features**
- ✅ Mining start/stop
- ✅ Mining address configuration
- ✅ Hash rate display

---

## 🔍 GUI Integration Checklist

### **✅ Overview Tab**
```cpp
// File: gui/src/mainwindow.cpp
- [ ] Shows blockchain height
- [ ] Shows sync progress
- [ ] Shows connections (peer count)
- [ ] Shows current phase (CPU-friendly/Halving)
- [ ] Shows total supply
- [ ] Shows next reward
```

### **✅ Wallet Tab**
```cpp
// File: gui/src/mainwindow.cpp
- [ ] Create HD wallet (BIP39 mnemonic)
- [ ] Generate new addresses (din1...)
- [ ] Display balance (confirmed/unconfirmed)
- [ ] Encrypt wallet
- [ ] Lock/unlock wallet
- [ ] Export seed phrase
```

### **✅ Send Tab**
```cpp
// File: gui/src/mainwindow.cpp
- [ ] Enter recipient address
- [ ] Enter amount
- [ ] Calculate fee
- [ ] Send transaction
- [ ] Show transaction result
```

### **✅ Mining Tab**
```cpp
// File: gui/src/mainwindow.cpp or minercontroller
- [ ] Start/stop mining
- [ ] Set mining address
- [ ] Show hash rate
- [ ] Show blocks found
```

---

## 🚨 Potential Issues to Check

### **1. Wallet Integration**
**Issue:** GUI might be using old wallet API  
**Check:**
```cpp
// Does it use HDWallet class?
// Does it call createhdwallet RPC?
// Does it use m/84'/1447'/0'/0/x path?
```

### **2. Address Format**
**Issue:** GUI might expect old address format  
**Check:**
```cpp
// Does it handle din1... bech32 addresses?
// Does it validate address format correctly?
```

### **3. RPC Methods**
**Issue:** GUI might call outdated RPC methods  
**Check:**
```cpp
// Does it use latest RPC methods?
// createhdwallet, restorewallet, getnewaddress, etc.
```

### **4. Network Info**
**Issue:** GUI might not show coin type or derivation path  
**Check:**
```cpp
// Does it display "Dinero (DIN)"?
// Does it show "Coin Type: 1447"?
// Does it show "m/84'/1447'/0'/0/x"?
```

---

## 📋 Testing Plan

### **Phase 1: Connection** (5 min)
1. Start daemon: `./build/dinerod`
2. Start GUI: `./gui/build/dinero-qt`
3. Verify: Green connection indicator

### **Phase 2: Wallet** (10 min)
1. Create new wallet
2. Get mnemonic (should be 12 words)
3. Generate address (should start with din1)
4. Encrypt wallet
5. Lock/unlock

### **Phase 3: Transactions** (10 min)
1. Mine some blocks
2. Check balance
3. Send transaction
4. Verify in mempool

### **Phase 4: Mining** (5 min)
1. Start miner
2. Check hash rate
3. Find block
4. Stop miner

---

## 🔧 Quick Fixes Needed

### **1. Update Window Title**
```cpp
// gui/src/main.cpp or mainwindow.cpp
app.setApplicationName("Dinero");        // ✅ Already done
app.setWindowTitle("Dinero Wallet");     // Check this
```

### **2. Update About Dialog**
```cpp
// Should show:
// - Dinero (not DineroCoin)
// - Coin Type: 1447
// - Derivation Path: m/84'/1447'/0'/0/x
// - Version with security fixes
```

### **3. Update Wallet Tab**
```cpp
// Should show:
// - "Generate Address" (not "New Address")
// - "Derivation: m/84'/1447'/0'/0/x"
// - "Address Index: 0, 1, 2..."
```

---

## 🎯 Next Steps

1. ⏳ **Test current GUI** - Launch and verify
2. ⏳ **Fix any issues** - Update RPC calls if needed
3. ⏳ **Add missing info** - Show coin type, derivation path
4. ⏳ **Polish UI** - Update branding to "Dinero"
5. ⏳ **Rebuild** - With all fixes applied

---

## 📊 Current Status

**GUI Binary:** `gui/build/dinero-qt` (386 KB)  
**Last Built:** Oct 6, 10:26  
**Needs Rebuild:** Probably yes (to include latest changes)

**Source Files:**
- `gui/src/main.cpp`
- `gui/src/mainwindow.cpp`
- `gui/src/rpcclient.cpp`
- `gui/src/walletwizard.cpp`
- `gui/src/minercontroller.cpp`

---

**Next:** Test the GUI and document what works/what needs fixing!

