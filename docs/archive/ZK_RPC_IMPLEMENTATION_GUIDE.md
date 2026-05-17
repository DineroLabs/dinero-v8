# ZK RPC Implementation Guide

**File:** `src/rpc/zk_rpc_handlers_context.cpp` (skeleton created, ready to implement)

---

## 📋 Summary

**Do you need to create RPC methods?**
- **Not immediately for Phase A** - Focus on core ZK library integration first
- **Yes, for Phase B** - Users need a way to create confidential transactions
- **Skeleton already created** - Just fill in implementations as you complete each phase

---

## 🎯 Implementation Timeline

| RPC Method | Phase | Priority | Purpose |
|------------|-------|----------|---------|
| `zk.verify` | Phase A | Testing | Verify commitment balance (for unit tests) |
| `zk.getcommitment` | Phase A | Helper | Get commitment from UTXO (helper method) |
| `zk.createtx` | Phase B | **Critical** | Create confidential TXs (main feature!) |
| `zk.verifyrangeproof` | Phase B | Testing | Verify range proofs (for testing) |
| `zk.scanviewkey` | Phase C | Usability | Recover amounts with view key |

---

## 📝 What You Need to Do

### Phase A (Commitments Only)
**Optional:** Implement `zk.verify` for testing
```cpp
din::Json rpc_context_zk_verify(const ExecutionContext& ctx, const din::Json& params) {
    // 1. Parse transaction hex
    // 2. Extract commitments
    // 3. Call secp256k1_pedersen_verify_tally()

    din::Json result;
    result["valid"] = true;  // or false
    result["balance_verified"] = true;
    return result;
}
```

**When:** After you have working Pedersen commitment code
**Why:** Useful for unit testing, but not required for production

---

### Phase B (Range Proofs)
**REQUIRED:** Implement `zk.createtx`
```cpp
din::Json rpc_context_zk_createtx(const ExecutionContext& ctx, const din::Json& params) {
    // 1. Parse inputs/outputs from JSON
    // 2. Generate blinding factors
    // 3. Create Pedersen commitments (Phase A)
    // 4. Create Bulletproof range proofs (Phase B)
    // 5. Build transaction
    // 6. Return hex + metadata

    din::Json result;
    result["hex"] = "transaction hex...";
    result["txid"] = "...";
    result["commitments"] = din::Json::array();  // blinding factors, nonces
    return result;
}
```

**When:** Phase B complete (range proofs working)
**Why:** This is how users will actually create confidential transactions

---

### Phase C (View Keys)
**REQUIRED:** Implement `zk.scanviewkey`
```cpp
din::Json rpc_context_zk_scanviewkey(const ExecutionContext& ctx, const din::Json& params) {
    // 1. Parse view key and block range
    // 2. Scan blockchain for confidential outputs
    // 3. Try to rewind range proofs with view key
    // 4. Return discovered outputs with amounts

    din::Json result;
    result["outputs"] = din::Json::array();
    result["total_received"] = 0;
    return result;
}
```

**When:** Users need to receive confidential payments
**Why:** Without this, receivers can't see amounts sent to them!

---

## 🔧 Integration Steps

### 1. Add to CMakeLists.txt
```cmake
# src/CMakeLists.txt or wherever RPC sources are listed
add_library(dinero_rpc STATIC
    # ... existing files ...
    src/rpc/zk_rpc_handlers_context.cpp  # ADD THIS
)
```

### 2. Register in rpc_context_wiring.cpp
```cpp
// Forward declaration
void WireZkRpcContext();

// In WireRpcContext() function:
void WireRpcContext() {
    // ... existing registrations ...

    WireZkRpcContext();
    dinero::g_logger.info("[RPC Context] ✅ ZK privacy context-aware handlers registered");
}
```

### 3. Uncomment includes as you implement
The skeleton file has commented includes:
```cpp
// TODO: Add these includes when ZK code is implemented
// #include "zk/confidential_tx.h"
// #include "zk/zk_validation.h"
// #include <secp256k1.h>
// #include <secp256k1_generator.h>
// #include <secp256k1_rangeproof.h>
```

Uncomment them as you create the corresponding code.

---

## 🧪 Testing RPC Methods

### Manual Testing (dinero-cli)
```bash
# Phase A: Verify commitment balance
dinero-cli zk.verify '{"txhex": "010000..."}'

# Phase B: Create confidential transaction
dinero-cli zk.createtx '{
  "inputs": [{"txid": "abc...", "vout": 0, "amount": 100000000}],
  "outputs": [{"address": "dinero1q...", "amount": 50000000, "confidential": true}]
}'

# Phase C: Scan with view key
dinero-cli zk.scanviewkey '{
  "viewkey": "deadbeef...",
  "start_height": 0,
  "end_height": 1000
}'
```

### Unit Testing
```cpp
// tests/test_zk_rpc.cpp
TEST(ZkRPC, CreateConfidentialTx) {
    // Set up test inputs/outputs
    din::Json params;
    params["inputs"] = din::Json::array();
    // ... add test data ...

    // Call RPC method
    ExecutionContext ctx;
    auto result = rpc_context_zk_createtx(ctx, params);

    // Verify result
    EXPECT_TRUE(result.contains("hex"));
    EXPECT_TRUE(result.contains("txid"));
}
```

---

## 📊 RPC Method Details

### `zk.createtx` (Most Important!)

**Input:**
```json
{
  "inputs": [
    {
      "txid": "previous_tx_id",
      "vout": 0,
      "amount": 100000000,              // Sender knows this
      "blinding_factor": "hex..."       // From previous output
    }
  ],
  "outputs": [
    {
      "address": "dinero1q...",
      "amount": 50000000,                // Will be hidden!
      "confidential": true
    }
  ],
  "fee": 10000
}
```

**Output:**
```json
{
  "hex": "serialized_tx_hex",
  "txid": "new_tx_id",
  "commitments": [
    {
      "vout": 0,
      "commitment": "33_byte_hex",       // Store on blockchain
      "blinding_factor": "secret_hex",   // Keep secret! Needed for spending
      "nonce": "32_byte_hex",            // Give to receiver (for amount recovery)
      "rangeproof_size": 5134
    }
  ],
  "verify": {
    "balance": true,
    "range_proofs": true
  }
}
```

**Critical:** User must save `blinding_factor` and `nonce` from output!
- Sender needs `blinding_factor` to spend in future
- Receiver needs `nonce` to see the amount

---

### `zk.scanviewkey` (For Receivers)

**Input:**
```json
{
  "viewkey": "32_byte_nonce_hex",
  "start_height": 0,
  "end_height": 1000
}
```

**Output:**
```json
{
  "outputs": [
    {
      "txid": "abc123...",
      "vout": 0,
      "block_height": 100,
      "amount": 50000000,           // Recovered from proof!
      "blinding_factor": "hex...",  // Recovered (for spending)
      "address": "dinero1q...",
      "spent": false
    }
  ],
  "total_received": 150000000,
  "blocks_scanned": 1001
}
```

**How it works:**
1. Receiver gets `nonce` from sender (out-of-band)
2. Calls `zk.scanviewkey` with the nonce
3. For each confidential output, tries to rewind range proof
4. If successful, recovers amount and blinding factor
5. Now receiver knows what they got!

---

## ⚠️ Security Considerations

### Blinding Factor Management
```cpp
// ❌ WRONG: Don't store in plaintext
Json result;
result["blinding_factor"] = blind_hex;

// ✅ RIGHT: Encrypt or use wallet database
WalletDB::StoreBlindingFactor(txid, vout, blind, encrypted=true);
```

### Nonce Distribution
```cpp
// Sender needs to give nonce to receiver somehow
// Options:
// 1. Embed in address (stealth address - Phase C)
// 2. Send via secure channel (Signal, PGP email)
// 3. Derive from shared secret (ECDH - Phase C)

// For Phase B, just return it to user:
result["nonce"] = nonce_hex;  // User must give to receiver manually
```

---

## ✅ Quick Checklist

**Phase A (Testing Only):**
- [ ] Skeleton file already created ✅
- [ ] Optionally implement `zk.verify` for testing
- [ ] NOT required for production

**Phase B (REQUIRED):**
- [ ] Implement `zk.createtx` - main feature!
- [ ] Implement `zk.verifyrangeproof` for testing
- [ ] Add to CMakeLists.txt
- [ ] Register in rpc_context_wiring.cpp
- [ ] Test with dinero-cli

**Phase C (User Experience):**
- [ ] Implement `zk.scanviewkey` - receivers need this!
- [ ] Implement stealth address generation
- [ ] Automatic nonce derivation

---

## 💡 Recommended Approach

1. **Focus on Phase A core library first** (you're doing this now!)
   - Get Pedersen commitments working
   - Write unit tests for commitment creation/verification
   - Skip RPC methods for now

2. **Implement `zk.createtx` in Phase B** (critical!)
   - After range proofs are working
   - This is how users create confidential TXs
   - Test manually with dinero-cli

3. **Add `zk.scanviewkey` in Phase C** (usability)
   - Receivers can't use confidential TXs without this
   - Implement automatic scanning

4. **Production hardening** (later)
   - Encrypt blinding factors in wallet DB
   - Automatic nonce management
   - Stealth addresses for privacy

---

## 🎉 Summary

**Answer to your question:**
- **Not required immediately** - Skeleton is ready, implement later
- **Most important:** `zk.createtx` (Phase B)
- **For users to receive:** `zk.scanviewkey` (Phase C)
- **Already created:** Skeleton file with all methods defined
- **Just fill in:** Implementation code as you complete each phase

Focus on Phase A core library now. RPC methods are ready when you need them!
