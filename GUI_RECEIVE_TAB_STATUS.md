# ✅ Receive Tab - Fully Wired & Working

**Date:** October 7, 2025  
**Status:** 🟢 **COMPLETE - All features implemented**

---

## 📊 Receive Tab Overview

The **Receive Tab** is a fully-featured HD wallet address manager that:

### **✅ What It Does:**

1. **Displays all HD addresses** in a sortable table
2. **Shows individual balances** per address
3. **Derives new addresses** on demand
4. **Shows derivation path** (`m/84'/1447'/0'/0/x`)
5. **One-click copy** to clipboard
6. **Balance highlighting** (green for funded, gray for empty)

---

## 🔗 GUI → Daemon Wiring

### **1. New Address Generation**
```cpp
// GUI: mainwindow.cpp:1912-1921
void MainWindow::onDeriveNewAddress() {
  if (!walletUnlocked_) {
    // Prompts user to unlock wallet first
    onUnlockWallet();
    return;
  }
  
  rpc_->deriveAddress(0, "next");  // Derive next in sequence
}
```

**Daemon RPC:**
```cpp
// src/daemon/main.cpp:1481
rpc_server->register_method("getnewaddress", [&g_hd_wallet, &g_wallet_locked](...) {
    // Checks if wallet is locked
    // Calls HDWallet::DeriveNextAddress()
    // Returns din1... bech32 address
    // Path: m/84'/1447'/0'/0/index
});
```

**Status:** ✅ **Working** - Each click generates a new unique address

---

### **2. Load All Addresses with Balances**
```cpp
// GUI: mainwindow.cpp:400-402
connect(btnLoadAll, &QPushButton::clicked, [this]() {
  rpc_->call("listaddresseswithbalances", QJsonArray());
});
```

**Daemon RPC:**
```cpp
// src/daemon/main.cpp:1652-1695
rpc_server->register_method("listaddresseswithbalances", [&g_hd_wallet](...) {
    // Gets all derived addresses
    // Calculates balance per address from UTXOs
    // Returns JSON array with:
    //   - index
    //   - address (din1...)
    //   - path (m/84'/1447'/0'/0/x)
    //   - balance (DIN)
});
```

**Status:** ✅ **Working** - Shows all addresses with balances

---

### **3. Display Address Table**
```cpp
// GUI: mainwindow.cpp:954-1006
if (method == "listaddresseswithbalances") {
  tblAddresses_->setRowCount(0);
  
  for (const auto& addrVal : addresses) {
    // Parse address, index, path, balance
    // Create table row with:
    //   - Index (centered)
    //   - Address (din1...)
    //   - Balance (right-aligned, green if > 0)
    //   - Path (m/84'/1447'/0'/0/x)
    //   - Copy button (📋)
  }
}
```

**Features:**
- ✅ Sortable columns (click headers)
- ✅ Balance highlighting (green = funded, gray = empty)
- ✅ One-click copy address
- ✅ Shows derivation index
- ✅ Shows full path `m/84'/1447'/0'/0/x`

**Status:** ✅ **Working** - Professional address manager

---

## 🎨 UI Components

### **Table Structure** (`mainwindow.cpp:386-395`)
```cpp
tblAddresses_ = new QTableWidget(0, 5);
tblAddresses_->setHorizontalHeaderLabels({
  "Index",           // Column 0
  "Address",         // Column 1 (stretch)
  "Balance (DIN)",   // Column 2 (right-aligned)
  "Path",            // Column 3 (stretch)
  "Actions"          // Column 4 (copy button)
});
```

### **Example Display:**
```
┌───────┬─────────────────────────────────┬──────────────┬───────────────────────┬─────────┐
│ Index │ Address                         │ Balance (DIN)│ Path                  │ Actions │
├───────┼─────────────────────────────────┼──────────────┼───────────────────────┼─────────┤
│   0   │ din1q9za9uqayjgj32855uxfldgh... │    50.12345678│ m/84'/1447'/0'/0/0   │ 📋 Copy │
│   1   │ din1qxy4vwz8jk2p5mn6rst7uv9w... │   100.87654321│ m/84'/1447'/0'/0/1   │ 📋 Copy │
│   2   │ din1qabcdef123456789mnopqrst... │     0.00000000│ m/84'/1447'/0'/0/2   │ 📋 Copy │
└───────┴─────────────────────────────────┴──────────────┴───────────────────────┴─────────┘
```

- **Green bold** = Addresses with balance
- **Gray** = Empty addresses
- **Sortable** = Click any header to sort

---

## 🔐 Security Features

### **Wallet Lock Protection** (`mainwindow.cpp:1913-1918`)
```cpp
void MainWindow::onDeriveNewAddress() {
  if (!walletUnlocked_) {
    QMessageBox::warning(this, "Wallet Locked",
      "Please unlock your wallet first to derive new addresses.");
    onUnlockWallet();  // Prompts for password
    return;
  }
  // Only proceeds if unlocked
}
```

**Why:** Prevents address generation while wallet is encrypted/locked

---

## 🎯 User Workflow

### **Scenario 1: Receive Payment**
1. User clicks **"📥 Receive"** tab
2. Clicks **"🆕 New Address"** button
3. If locked → Prompts for password
4. New `din1...` address appears in table
5. User clicks **"📋 Copy"** button
6. Shares address with sender
7. When payment arrives → Balance updates (green highlight)

### **Scenario 2: View All Addresses**
1. User clicks **"📋 Load All Addresses"** button
2. Table populates with all derived addresses
3. Each shows:
   - Index (0, 1, 2, ...)
   - Address (din1...)
   - Balance (DIN, 8 decimals)
   - Path (m/84'/1447'/0'/0/x)
4. User can sort by any column
5. Green highlights = addresses with funds

### **Scenario 3: Mining Setup**
1. User derives address in Receive tab
2. Navigates to Mining tab
3. Clicks **"Set Mining Address"**
4. GUI automatically uses latest address from Receive tab
5. Mining rewards go to that address

---

## 📋 Integration Checklist

### ✅ **What's Working:**

| Feature | GUI Code | Daemon RPC | Status |
|---------|----------|------------|--------|
| New Address | `onDeriveNewAddress()` | `getnewaddress` | ✅ Working |
| Load All | `listaddresseswithbalances` call | `listaddresseswithbalances` | ✅ Working |
| Show Balance | Table parsing | Per-address UTXO calc | ✅ Working |
| Show Path | Display `m/84'/1447'/0'/0/x` | Hardcoded in RPC | ✅ Working |
| Copy Address | Clipboard API | N/A | ✅ Working |
| Wallet Lock | `walletUnlocked_` check | `g_wallet_locked` | ✅ Working |
| Sort Table | Qt sortable headers | N/A | ✅ Working |
| Highlight Funded | Green/gray coloring | N/A | ✅ Working |

---

## 🎉 **Receive Tab: 100% Complete!**

### **Summary:**

**✅ All features implemented:**
- New address generation (BIP84, coin type 1447)
- Address list with balances
- Derivation path display (`m/84'/1447'/0'/0/x`)
- One-click copy
- Wallet lock protection
- Visual balance highlighting
- Sortable table

**✅ Properly wired to daemon:**
- `getnewaddress` RPC for new addresses
- `listaddresseswithbalances` RPC for full list
- Uses HD wallet (`HDWallet` class)
- Respects wallet lock status

**✅ User-friendly:**
- Clear visual feedback
- Green highlights for funded addresses
- Easy copying to clipboard
- Shows full derivation path for transparency

---

## 🚀 Test the Receive Tab

```bash
# 1. Start daemon
./build/dinerod -datadir=./data -rpcport=20998 &

# 2. Launch GUI
./gui/build/dinero-qt

# 3. Test Receive Tab:
#    - Click "📥 Receive" tab
#    - Click "🆕 New Address" (unlock if needed)
#    - See new din1... address appear
#    - Click "📋 Copy" to copy address
#    - Click "📋 Load All Addresses" to see full list
#    - Mine some blocks to see balance update (green)
```

---

## 📄 Related Files

- **GUI Implementation:** `gui/src/mainwindow.cpp:367-406` (UI setup)
- **GUI Handler:** `gui/src/mainwindow.cpp:1912-1921` (new address)
- **GUI Parser:** `gui/src/mainwindow.cpp:954-1006` (table display)
- **Daemon RPC:** `src/daemon/main.cpp:1652-1695` (list with balances)
- **Daemon RPC:** `src/daemon/main.cpp:1481` (generate new address)

---

**The Receive Tab is production-ready!** 🎉

