# Mining-Wallet Integration Implementation Plan ⛏️

**Date**: January 2025  
**Status**: ✅ **READY TO IMPLEMENT**

---

## ✅ I Understand How To Implement All Features

### 1. HDWallet-Derived Mining Addresses ✅

**What I Know**:
- `HDWallet` has `DeriveAddressAt(uint32_t index)` for receive chain (m/84'/1447'/0'/0/index)
- `HDWallet` has `DeriveChangeAddressAt(uint32_t index)` for change chain (m/84'/1447'/0'/1/index)
- `HDWallet` has `GetPrivateKeyAt(uint32_t index)` for private key access
- Need to add mining chain: m/84'/1447'/0'/2/index

**Implementation Plan**:
```cpp
// In include/wallet/hd_wallet.h
std::string DeriveNextMiningAddress();  // Returns din1… from mining chain
uint32_t CurrentMiningIndex() const;
std::string GetMiningAddressAt(uint32_t index);
std::vector<uint8_t> GetMiningPrivateKeyAt(uint32_t index) const;

// In src/wallet/hd_wallet.cpp
// Add mining_index_ member variable
// Implement DeriveMiningAddressAt() using BIP32 path m/84'/1447'/0'/2/index
// Store mining address index in wallet file
```

**Integration Points**:
- Update `mining.setaddress` RPC to optionally derive from wallet
- Add `mining.deriveaddress` RPC that derives from HDWallet
- Store mining address index in WalletManager settings

---

### 2. Automatic Coinbase Indexing ✅

**What I Know**:
- `BlockAcceptor::ProcessBlock()` processes blocks and marks coinbase: `coin.coinbase = (tx_idx == 0)`
- `WalletManager::addUTXO()` exists and handles coinbase maturity (100 blocks)
- Need to hook into block processing to call `addUTXO()` for coinbase transactions

**Implementation Plan**:
```cpp
// In src/daemon/block_acceptor.cpp - after block is accepted
// After ProcessBlock() succeeds, notify wallet manager:

if (g_wallet_manager && tx_idx == 0) {  // Coinbase transaction
    // Parse coinbase output
    for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
        std::string address = ExtractAddressFromScriptPubKey(tx.vout[vout].scriptPubKey);
        if (g_wallet_manager->isAddressMine(address)) {
            g_wallet_manager->addUTXO(
                txid, vout, 
                tx.vout[vout].value,
                address,
                HexEncode(tx.vout[vout].scriptPubKey),
                height,
                true  // is_coinbase
            );
        }
    }
}
```

**Integration Points**:
- Hook into `BlockAcceptor::ProcessBlock()` success path
- Check if coinbase output address is registered with wallet
- Call `WalletManager::addUTXO()` with `is_coinbase=true`
- Balance will automatically update as maturity is tracked

---

### 3. Auto-payout Feature ✅

**What I Know**:
- Command-line parsing in `parse_args()` function
- `WalletManager::addUTXO()` tracks maturity
- `WalletManager::getAvailableUTXOs()` filters mature coinbase
- `HDWallet::CreateTransaction()` can send transactions

**Implementation Plan**:
```cpp
// In src/daemon/main.cpp Config struct
bool autopayout = false;  // Add to Config

// In parse_args()
config.autopayout = opts.has("autopayout");

// Background thread (after RPC server starts)
if (config.autopayout) {
    std::thread autopayout_thread([&]() {
        while (!shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::minutes(10));  // Check every 10 min
            
            if (!g_wallet_manager || !g_hd_wallet || !(*g_hd_wallet)) continue;
            
            // Get mature coinbase UTXOs
            auto utxos = g_wallet_manager->listUnspentUTXOs(100, 999999);
            std::vector<WalletManager::UTXO> mature_coinbase;
            for (const auto& utxo : utxos) {
                if (utxo.is_coinbase && utxo.confirmations >= 100) {
                    mature_coinbase.push_back(utxo);
                }
            }
            
            if (mature_coinbase.empty()) continue;
            
            // Get main account address (first receive address)
            std::string main_address = (*g_hd_wallet)->GetAddressAt(0);
            
            // Calculate total amount
            uint64_t total_amount = 0;
            for (const auto& utxo : mature_coinbase) {
                total_amount += utxo.amount_una;
            }
            
            // Create transaction sending to main address
            HDWallet::TxOutput output;
            output.address = main_address;
            output.value = total_amount - 1000;  // Leave fee
            
            std::string tx_hex, error;
            if ((*g_hd_wallet)->CreateTransaction({output}, 1000, tx_hex, error)) {
                // Broadcast transaction
                // Mark UTXOs as spent
            }
        }
    });
    autopayout_thread.detach();
}
```

---

### 4. Comprehensive Wallet RPC Tests ✅

**What I Know**:
- Test script structure from `test_mining_wallet_integration.sh`
- RPC authentication pattern
- Need to test full flow

**Implementation Plan**:
```bash
#!/bin/bash
# test_mining_wallet_comprehensive.sh

# 1. Create wallet
createhdwallet

# 2. Get mining address from wallet
ADDRESS=$(wallet.deriveminingaddress)

# 3. Set mining address
mining.setaddress $ADDRESS

# 4. Start mining
mining.start 1

# 5. Mine block (if regtest) or wait for block
generatetoaddress 1 $ADDRESS

# 6. Check balance (should show immature coinbase)
getbalance

# 7. Wait for maturity (100 blocks) or fast-forward
generatetoaddress 100 $ADDRESS

# 8. Check balance (should show mature coinbase)
getbalance

# 9. List UTXOs
listunspent

# 10. Spend mining reward
sendtoaddress $MAIN_ADDRESS 50.0

# 11. Verify transaction
listtransactions
```

---

## 🎯 Implementation Order

1. **HDWallet Mining Address Derivation** (1-2 hours)
   - Add mining chain derivation
   - Add `DeriveNextMiningAddress()` method
   - Update RPC handlers

2. **Coinbase Indexing** (1-2 hours)
   - Hook into block processing
   - Call `addUTXO()` for coinbase transactions
   - Test balance updates

3. **Comprehensive Tests** (1 hour)
   - Create full flow test script
   - Test all scenarios

4. **Auto-payout Feature** (2-3 hours)
   - Add command-line flag
   - Implement background monitor
   - Test auto-payout logic

**Total Estimated Time**: 5-8 hours

---

## ✅ Ready to Implement

I understand:
- ✅ HDWallet structure and BIP32 derivation
- ✅ Block processing flow and coinbase detection
- ✅ UTXO indexing and maturity tracking
- ✅ Transaction creation and broadcasting
- ✅ Test script patterns

**Should I proceed with implementation?**

