# Phase 3: Transaction Spending & Fee Logic

**Status:** Planning / Implementation Ready
**Prerequisites:** ✅ Wallet persistence, ✅ ASERT difficulty, ✅ Mining stability
**Goal:** Enable users to create, sign, and broadcast spending transactions

---

## Overview

Phase 3 focuses on the full transaction lifecycle:
1. **Create** spending transactions from wallet UTXOs
2. **Sign** transactions with correct witness data
3. **Broadcast** to network and mempool
4. **Fee estimation** for reasonable confirmation times
5. **Change output** handling and UTXO management

---

## Current Status Assessment

### What Already Works ✅
- Wallet can receive coinbase transactions
- UTXO set is persisted correctly in RocksDB
- Wallet tracks balance and UTXOs
- Mining creates valid coinbase transactions
- Block validation accepts transactions

### What Needs Implementation 🔨

#### 1. Coinbase Maturity Enforcement
**File:** `src/consensus/validation.cpp` or `src/daemon/block_acceptor.cpp`

```cpp
// Coinbase outputs cannot be spent until 100 blocks deep
const uint32_t COINBASE_MATURITY = 100;

bool IsCoinbaseSpendable(const Coin& coin, uint32_t current_height) {
    if (!coin.is_coinbase) return true;  // Non-coinbase always spendable
    return (current_height - coin.height) >= COINBASE_MATURITY;
}
```

**Why:** Prevents spending coins from orphaned blocks.

---

#### 2. Transaction Creation (Wallet)
**File:** `src/wallet/wallet_manager.cpp` - `createTransaction()` method

**Inputs:**
- Recipient address (Bech32 P2WPKH)
- Amount to send (una)
- Fee rate (una/vByte) - optional, use estimator

**Process:**
1. Select UTXOs (coin selection algorithm)
2. Calculate required input amount (amount + fees)
3. Create transaction inputs (prevout + scriptSig placeholder)
4. Create outputs:
   - Payment output (to recipient)
   - Change output (back to sender's wallet)
5. Calculate witness size and adjust fees if needed

**Coin Selection Strategy:**
```cpp
// Start with simple "largest first" selection
// Later: implement Branch-and-Bound for privacy
std::vector<UTXO> SelectCoins(uint64_t target_amount, uint64_t fee_rate) {
    std::vector<UTXO> selected;
    uint64_t total = 0;

    // Sort UTXOs by value (largest first)
    auto utxos = wallet->GetSpendableUTXOs();
    std::sort(utxos.begin(), utxos.end(),
        [](const UTXO& a, const UTXO& b) { return a.value > b.value; });

    // Greedy selection
    for (const auto& utxo : utxos) {
        selected.push_back(utxo);
        total += utxo.value;

        uint64_t estimated_fee = CalculateFee(selected.size(), 2, fee_rate);
        if (total >= target_amount + estimated_fee) {
            return selected;  // Sufficient funds
        }
    }

    throw InsufficientFunds();
}
```

---

#### 3. Transaction Signing
**File:** `src/wallet/transaction_signing.cpp` (new file)

**BIP143 SegWit Signing:**
```cpp
// For each input, compute witness signature
std::vector<uint8_t> SignInput(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& scriptPubKey,
    uint64_t amount,
    const std::vector<uint8_t>& private_key) {

    // 1. Compute BIP143 sighash
    auto sighash = CalculateBIP143Sighash(tx, input_index, scriptPubKey, amount);

    // 2. Sign with ECDSA
    auto signature = secp256k1_sign(private_key, sighash);

    // 3. Append SIGHASH_ALL byte
    signature.push_back(0x01);  // SIGHASH_ALL

    return signature;
}

// BIP143 sighash calculation (SegWit)
uint256 CalculateBIP143Sighash(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& scriptCode,  // For P2WPKH: OP_DUP OP_HASH160 <pubkeyhash> OP_EQUALVERIFY OP_CHECKSIG
    uint64_t amount) {

    // Double SHA256 of:
    // 1. nVersion (4 bytes)
    // 2. hashPrevouts (32 bytes) - hash of all input outpoints
    // 3. hashSequence (32 bytes) - hash of all input sequences
    // 4. outpoint (36 bytes) - current input's prevout
    // 5. scriptCode (varies) - witness program
    // 6. amount (8 bytes) - input amount
    // 7. nSequence (4 bytes) - current input sequence
    // 8. hashOutputs (32 bytes) - hash of all outputs
    // 9. nLockTime (4 bytes)
    // 10. sighash type (4 bytes) - SIGHASH_ALL = 0x01000000

    return DoubleSHA256(/* concatenated fields */);
}
```

**Witness Structure (P2WPKH):**
```cpp
// For each input, witness contains:
// [<signature> <pubkey>]
tx.wit.vtxinwit[input_index] = {
    signature,  // 71-73 bytes typically
    pubkey      // 33 bytes (compressed)
};
```

---

#### 4. Fee Estimation
**File:** `src/mempool/fee_estimator.cpp` (new file)

**Simple Strategy (Phase 3):**
```cpp
// Conservative static fee rates (una per virtual byte)
const uint64_t FEE_RATE_HIGH = 10;    // ~10 minutes (1-2 blocks)
const uint64_t FEE_RATE_MEDIUM = 5;   // ~20 minutes (2-4 blocks)
const uint64_t FEE_RATE_LOW = 1;      // ~1 hour (10+ blocks)

uint64_t EstimateFee(size_t tx_vsize, FeeLevel level) {
    uint64_t rate = (level == HIGH) ? FEE_RATE_HIGH :
                    (level == MEDIUM) ? FEE_RATE_MEDIUM : FEE_RATE_LOW;
    return tx_vsize * rate;
}
```

**Advanced Strategy (Post-Phase 3):**
- Track mempool congestion
- Historical confirmation time analysis
- Dynamic fee bumping (RBF)

**Virtual Size Calculation:**
```cpp
// SegWit weight units: base × 3 + total × 1
uint64_t CalculateVirtualSize(const Transaction& tx) {
    uint64_t base_size = tx.GetBaseSize();      // Without witness
    uint64_t total_size = tx.GetTotalSize();    // With witness
    uint64_t weight = base_size * 3 + total_size;
    return (weight + 3) / 4;  // Round up
}
```

---

#### 5. Change Output Handling
**File:** `src/wallet/wallet_manager.cpp` - `createTransaction()`

**Change Calculation:**
```cpp
uint64_t CalculateChange(
    uint64_t total_input,
    uint64_t payment_amount,
    uint64_t fee) {

    if (total_input < payment_amount + fee) {
        throw InsufficientFunds();
    }

    uint64_t change = total_input - payment_amount - fee;

    // Dust threshold: don't create uneconomical outputs
    const uint64_t DUST_THRESHOLD = 546;  // una
    if (change > 0 && change < DUST_THRESHOLD) {
        // Add dust to fee instead of creating tiny output
        return 0;
    }

    return change;
}
```

**Change Address:**
```cpp
// For privacy, generate new change address from keypool
std::string GetChangeAddress(Wallet* wallet) {
    // Option 1: Reuse existing address (simpler, less private)
    return wallet->GetDefaultAddress();

    // Option 2: Generate new address from HD path (better privacy)
    return wallet->DeriveNewAddress(/* change path: m/84'/0'/0'/1/n */);
}
```

---

#### 6. Mempool Acceptance Validation
**File:** `src/daemon/mempool.cpp` - `acceptToMempool()`

**Validation Checks:**
```cpp
bool AcceptToMempool(const Transaction& tx) {
    // 1. Format validation
    if (!tx.IsSane()) return false;  // Non-empty inputs/outputs, no negative values

    // 2. Coinbase check
    if (tx.IsCoinbase()) return false;  // Coinbase only in blocks

    // 3. Input existence check
    for (const auto& input : tx.vin) {
        if (!UTXOSet.Exists(input.prevout)) {
            return false;  // Spending non-existent UTXO
        }
    }

    // 4. Double-spend check
    if (mempool.HasConflict(tx)) return false;

    // 5. Signature validation
    if (!VerifySignatures(tx)) return false;

    // 6. Fee check
    uint64_t fee = CalculateFee(tx);
    if (fee < MIN_RELAY_FEE) return false;

    // 7. Add to mempool
    mempool.Add(tx);
    return true;
}
```

---

#### 7. Transaction Broadcasting
**File:** `src/daemon/p2p_manager.cpp` - `broadcastTransaction()`

**P2P Relay:**
```cpp
void BroadcastTransaction(const Transaction& tx) {
    // 1. Validate transaction locally
    if (!mempool.AcceptToMempool(tx)) {
        throw std::runtime_error("Transaction rejected by mempool");
    }

    // 2. Create INV message
    CInv inv(MSG_TX, tx.GetHash());

    // 3. Announce to all connected peers
    for (auto& peer : peers) {
        if (peer.SupportsTransaction()) {
            peer.SendMessage(CNetMessage::INV, {inv});
        }
    }

    LOG_INFO("Broadcasted transaction: " + tx.GetHash().ToString());
}
```

---

## Implementation Order (Recommended)

### Week 1: Foundation
1. Implement coinbase maturity check in block validation
2. Add `GetSpendableUTXOs()` method to wallet
3. Write unit tests for UTXO selection

### Week 2: Transaction Creation
4. Implement coin selection algorithm (greedy/largest-first)
5. Implement `createTransaction()` skeleton
6. Add change output logic

### Week 3: Signing
7. Implement BIP143 sighash calculation
8. Integrate secp256k1 signing
9. Build witness data structures
10. Test signature verification

### Week 4: Mempool & Broadcasting
11. Enhance mempool validation (double-spend detection)
12. Implement transaction broadcasting via P2P
13. Add fee estimation (static rates initially)
14. Integration tests (end-to-end spending)

---

## Testing Strategy

### Unit Tests
```cpp
TEST(TransactionCreation, CoinSelection) {
    // Given UTXOs: [100, 50, 25, 10]
    // Target: 60 + 5 fee = 65
    // Expected: Select [100] (sufficient)
}

TEST(TransactionSigning, BIP143Sighash) {
    // Known test vectors from BIP143
    // Verify sighash matches expected value
}

TEST(FeeEstimation, VirtualSize) {
    // P2WPKH transaction: 1 input, 2 outputs
    // Expected vsize: ~110 vbytes
}
```

### Integration Tests
```cpp
TEST(EndToEnd, SpendCoinbase) {
    // 1. Mine 101 blocks (mature coinbase)
    // 2. Create spend transaction
    // 3. Sign and broadcast
    // 4. Mine 1 block to confirm
    // 5. Verify recipient received funds
    // 6. Verify change returned to sender
}
```

### Regtest Workflow
```bash
# Mine maturity blocks
./bin/dinero-cli -regtest generatetoaddress 101 <miner-address>

# Check spendable balance
./bin/dinero-cli -regtest getbalance

# Create and send transaction
./bin/dinero-cli -regtest sendtoaddress <recipient> 50.0

# Mine confirmation block
./bin/dinero-cli -regtest generatetoaddress 1 <miner-address>

# Verify transaction confirmed
./bin/dinero-cli -regtest gettransaction <txid>
```

---

## Success Criteria

Phase 3 is complete when:

✅ Coinbase maturity (100 blocks) is enforced
✅ Wallet can create valid spending transactions
✅ Transactions are correctly signed (BIP143 SegWit)
✅ Mempool accepts and validates transactions
✅ Transactions broadcast to network peers
✅ Fees are calculated correctly (no dust outputs)
✅ Change outputs return to sender wallet
✅ End-to-end spending test passes on regtest

---

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Signature bug (funds lost) | Critical | Extensive test vectors from BIP143 |
| Fee miscalculation | High | Conservative initial fee rates |
| Double-spend in mempool | Medium | Strict conflict detection |
| Change address reuse | Privacy | Generate new addresses (HD wallet) |

---

## Future Enhancements (Post-Phase 3)

- **RBF (Replace-By-Fee):** Allow fee bumping for stuck transactions
- **CPFP (Child-Pays-For-Parent):** Spend unconfirmed outputs with higher fee
- **Batch transactions:** Multiple recipients in one transaction
- **Smart fee estimation:** Dynamic rates based on mempool congestion
- **Coin control:** Manual UTXO selection for privacy
- **Taproot support:** P2TR outputs (future soft fork)

---

## References

1. **BIP143 (Transaction Signature Verification for Version 0 Witness Program)**
   https://github.com/bitcoin/bips/blob/master/bip-0143.mediawiki

2. **BIP141 (Segregated Witness)**
   https://github.com/bitcoin/bips/blob/master/bip-0141.mediawiki

3. **Bitcoin Core Fee Estimation**
   https://github.com/bitcoin/bitcoin/blob/master/doc/estimatefee.md

4. **Coin Selection Algorithms**
   https://bitcoin.stackexchange.com/questions/1077/what-is-the-coin-selection-algorithm

---

**Document Status:** Living document - update as Phase 3 progresses
**Last Updated:** December 22, 2025
