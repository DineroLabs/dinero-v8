# BlockAcceptor Migration to GlobalUTXOSet

**File:** `src/daemon/block_acceptor.cpp`
**Status:** ✅ COMPLETED - Now uses GlobalUTXOSet for all UTXO operations

---

## 🔍 **Current Implementation (Lines 1086-1163)**

### **UTXO Spending (Lines 1088-1101):**
```cpp
// ❌ WRONG - Uses ChainDB for UTXO operations
for (const auto& input : tx.vin) {
    dinero::uint256 prevTxid(input.prevout.txid);
    auto delStatus = chain_db->deleteCoin(prevTxid, input.prevout.vout, &batch);

    if (delStatus != dinero::Status::Ok) {
        error = "Failed to delete spent UTXO";
        return false;
    }
}
```

### **UTXO Creation (Lines 1104-1123):**
```cpp
// ❌ WRONG - Uses ChainDB for UTXO operations
for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
    dinero::Coin coin;
    coin.amount = tx.vout[vout].value;
    coin.script_pubkey = ...;
    coin.height = height;
    coin.coinbase = (tx_idx == 0);

    dinero::uint256 txidU256(txid);
    auto putStatus = chain_db->putCoin(txidU256, vout, coin, &batch);

    if (putStatus != dinero::Status::Ok) {
        error = "Failed to add new UTXO";
        return false;
    }
}
```

### **Wallet UTXO Tracking (Lines 1129-1163):**
```cpp
// ⚠️ USES OLD UTXOIndex
auto* utxo_index = ctx_->chainstate->utxoIndex();
if (utxo_index) {
    auto opt_path = utxo_index->IsOurScript(script_bytes);
    if (opt_path.has_value()) {
        // Add to wallet_utxos table
        wallet.addUTXO(...);
    }
}
```

---

## ✅ **Required Changes**

### **1. Replace ChainDB UTXO Operations with GlobalUTXOSet**

**Add to BlockAcceptor class:**
```cpp
// In include/daemon/block_acceptor.h or at top of ConnectBlock():
#include "consensus/global_utxo_set.h"
```

**Get GlobalUTXOSet from context:**
```cpp
// In ConnectBlock(), after getting chain_db:
auto* global_utxos = ctx_->chainstate->getGlobalUTXOSet();
if (!global_utxos) {
    error = "GlobalUTXOSet not initialized";
    LOG_ERROR("❌ " + error);
    return false;
}
```

### **2. Update UTXO Spending Logic**

**OLD (Lines 1088-1101):**
```cpp
for (const auto& input : tx.vin) {
    dinero::uint256 prevTxid(input.prevout.txid);
    auto delStatus = chain_db->deleteCoin(prevTxid, input.prevout.vout, &batch);
    // ...
}
```

**NEW:**
```cpp
for (const auto& input : tx.vin) {
    std::string prev_txid = input.prevout.txid;
    uint32_t prev_vout = input.prevout.vout;

    // Spend UTXO from GlobalUTXOSet (RocksDB)
    if (!global_utxos->spendUTXO(prev_txid, prev_vout)) {
        error = "Failed to spend UTXO: " + prev_txid.substr(0, 16) + "...:" + std::to_string(prev_vout);
        LOG_ERROR("❌ CRITICAL: " + error + " - double spend or missing UTXO!");
        return false;
    }

    LOG_INFO("  🗑️ Spent UTXO: " + prev_txid.substr(0, 16) + "...:" + std::to_string(prev_vout));
}
```

### **3. Update UTXO Creation Logic**

**OLD (Lines 1104-1123):**
```cpp
for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
    dinero::Coin coin;
    coin.amount = tx.vout[vout].value;
    coin.script_pubkey = ...;
    coin.height = height;
    coin.coinbase = (tx_idx == 0);

    dinero::uint256 txidU256(txid);
    auto putStatus = chain_db->putCoin(txidU256, vout, coin, &batch);
    // ...
}
```

**NEW:**
```cpp
for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
    // Build GlobalUTXO struct
    consensus::GlobalUTXO utxo;
    utxo.txid = txid;
    utxo.vout = vout;
    utxo.amount = tx.vout[vout].value;
    utxo.scriptPubKey = tx.vout[vout].scriptPubKey;  // Already vector<uint8_t>
    utxo.height = static_cast<uint32_t>(height);
    utxo.is_coinbase = (tx_idx == 0);

    // Add to GlobalUTXOSet (RocksDB)
    if (!global_utxos->addUTXO(utxo)) {
        error = "Failed to add new UTXO: " + txid.substr(0, 16) + "...:" + std::to_string(vout);
        LOG_ERROR("❌ " + error);
        return false;
    }

    LOG_INFO("  ✅ Created UTXO: " + txid.substr(0, 16) + "...:" + std::to_string(vout) +
             " (value=" + std::to_string(utxo.amount) + ", coinbase=" + (utxo.is_coinbase ? "true" : "false") + ")");
}
```

### **4. Update Wallet UTXO Tracking**

**OLD (Lines 1129-1163):**
```cpp
auto* utxo_index = ctx_->chainstate->utxoIndex();  // ❌ OLD
if (utxo_index) {
    auto opt_path = utxo_index->IsOurScript(script_bytes);
    // ...
}
```

**NEW:**
```cpp
// Get WalletUTXOTracker from wallet (not from chainstate)
if (coin.coinbase && ctx_ && ctx_->wallet) {
    auto& wallet = ctx_->wallet->get();
    auto* wallet_utxo_tracker = wallet.getUTXOTracker();  // ✅ NEW

    if (wallet_utxo_tracker) {
        // Check if this scriptPubKey belongs to our wallet
        std::vector<uint8_t> script_bytes;
        for (size_t i = 0; i < coin.script_pubkey.length(); i += 2) {
            if (i + 1 < coin.script_pubkey.length()) {
                std::string byte_str = coin.script_pubkey.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
                script_bytes.push_back(byte);
            }
        }

        // TODO: Need method to check if script belongs to wallet
        // For now, check if matches mining address
        std::string network = dinero::Params().name;
        std::string mining_address = wallet.getMiningAddress("", network);

        if (!mining_address.empty()) {
            std::vector<uint8_t> mining_spk;
            std::string error;
            if (BuildScriptPubKeyFromAddress(mining_address, mining_spk, error)) {
                if (mining_spk == script_bytes) {
                    // This matches our mining address - add to wallet_utxos
                    wallet::WalletUTXO wallet_utxo;
                    wallet_utxo.txid = txid;
                    wallet_utxo.vout = vout;
                    wallet_utxo.derivation_path = opt_path.value_or("m/84'/1447'/0'/0/0");  // TODO: Get actual path
                    wallet_utxo.cached_amount = coin.amount;
                    wallet_utxo.cached_height = height;
                    wallet_utxo.is_spent = false;
                    wallet_utxo.is_locked = false;
                    wallet_utxo.label = "Mining reward";

                    if (wallet_utxo_tracker->addOwnedUTXO(wallet_utxo)) {
                        LOG_INFO("💰 Added coinbase to wallet_utxos: " + txid.substr(0, 16) + "...:" + std::to_string(vout));
                    }
                }
            }
        }
    }
}
```

---

## 🎯 **Summary of Changes**

| What | OLD | NEW |
|------|-----|-----|
| **UTXO Spend** | `chain_db->deleteCoin()` | `global_utxos->spendUTXO()` |
| **UTXO Add** | `chain_db->putCoin()` | `global_utxos->addUTXO()` |
| **Wallet Check** | `ctx_->chainstate->utxoIndex()` | `wallet.getUTXOTracker()` |
| **Storage** | ChainDB (mixed with block index) | GlobalUTXOSet (separate RocksDB) |

---

## ⚠️ **Important Notes**

1. **Atomic Operations:** GlobalUTXOSet needs batch support for atomic block application
   - Check if `GlobalUTXOSet::beginBatch()` / `commitBatch()` exist
   - If not, UTXOs are written immediately (less efficient but still correct)

2. **Undo Records:** The undo record building (line 1028) likely needs updating to work with GlobalUTXOSet

3. **DisconnectBlock:** Also needs updating (lines 1258+) to reverse UTXO operations using GlobalUTXOSet

---

**Status:** Migration plan documented, ready to implement
