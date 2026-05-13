# 🎨 Dinero Rebranding Plan

**From:** DineroCoin  
**To:** Dinero  
**Date:** October 7, 2025

---

## ✅ **Already Updated:**

1. ✅ **SLIP-44 Registration** - Submitted as "Dinero" (PR #1935)
2. ✅ **SDK Folder** - `sdk/ios/DineroKit/` (was DineroCoinKit)

---

## 📋 **What to Rename:**

### **High Priority (User-Facing):**

1. **SDK & Library Names:**
   - ✅ `DineroKit` (iOS) - Done
   - ⏳ `libdinero` (Android)
   - ⏳ `@dinero/sdk` (Web/npm)

2. **App Names:**
   - ⏳ "Dinero Wallet" (iOS)
   - ⏳ "Dinero Wallet" (Android)
   - ⏳ "Dinero" (GUI)

3. **Documentation:**
   - ⏳ README.md
   - ⏳ Website
   - ⏳ User guides

4. **Branding:**
   - ⏳ Logo files
   - ⏳ App icons
   - ⏳ Social media

### **Medium Priority (Internal):**

5. **Binary Names:**
   - Current: `dinerod`, `dinero-miner`, `dinero-qt`
   - Keep as-is (common in crypto: `bitcoind`, `litecoind`)

6. **Code Comments/Strings:**
   - ⏳ Update user-visible strings
   - ⏳ Keep internal variable names (low priority)

7. **Network Identifiers:**
   - Keep: `din` (Bech32 HRP)
   - Keep: `DIN` (ticker symbol)
   - Keep: Coin type 1447

### **Low Priority (Leave As-Is):**

8. **Repository Name:**
   - Keep: `/Documents/DineroCoin/` (avoid breaking paths)
   - Alternative: Symlink if needed

9. **Internal Namespaces:**
   - Keep: `namespace dinero` (C++ code)
   - Keep: Existing function names

10. **File/Directory Names:**
    - Keep: Most internal paths
    - Only rename user-facing files

---

## 🎯 **Quick Wins (Do Now):**

```bash
# 1. Update README title
sed -i '' 's/DineroCoin/Dinero/g' README.md

# 2. Update version string
sed -i '' 's/DineroCoin/Dinero/g' src/daemon/main.cpp

# 3. Update GUI window title
# In GUI code: "Dinero Wallet" instead of "DineroCoin"
```

---

## 📝 **Branding Consistency:**

| Context | Use |
|---------|-----|
| **Full Name** | Dinero |
| **Ticker** | DIN |
| **HRP** | din |
| **Daemon** | dinerod |
| **CLI** | dinero-cli |
| **GUI** | Dinero Wallet |
| **iOS SDK** | DineroKit |
| **Android** | Dinero SDK |
| **Web** | @dinero/sdk |

---

## 🚀 **SDK Naming (Final):**

```
Dinero Project Structure:
├── Core Daemon: dinerod
├── iOS SDK: DineroKit
├── Android SDK: Dinero SDK (libdinero.so)
├── Web SDK: @dinero/sdk (npm)
└── Wallets: "Dinero Wallet"
```

---

## ⚠️ **What NOT to Change:**

1. ❌ Repository folder name (avoid breaking paths)
2. ❌ Binary names (dinerod, dinero-cli, dinero-qt)
3. ❌ C++ namespaces (namespace dinero)
4. ❌ Network identifiers (din, DIN, 1447)
5. ❌ Internal variable names (low priority)

---

## 🎨 **Branding Guidelines:**

**Correct:**
- "Dinero" (standalone)
- "Dinero Wallet" (app name)
- "DineroKit" (SDK name)
- "DIN" (ticker)

**Avoid:**
- "DineroCoin" (old name)
- "Dinero Coin" (two words)
- "DINERO" (all caps, except ticker)

---

## 📊 **Impact Assessment:**

| Area | Impact | Effort |
|------|--------|--------|
| SDK Names | High | Low (already done) |
| Documentation | High | Medium |
| User Strings | High | Medium |
| Binary Names | Low | Keep as-is |
| Code/Internals | Low | Optional |

---

## ✅ **Next Steps:**

1. ⏳ Update README.md (5 min)
2. ⏳ Update daemon version string (5 min)
3. ⏳ Create iOS SDK with "Dinero" branding
4. ⏳ Update all documentation
5. ⏳ Community announcement with new name

---

**Summary:** Use "Dinero" for all user-facing content, keep technical names (dinerod, din) as-is for consistency with crypto conventions.

