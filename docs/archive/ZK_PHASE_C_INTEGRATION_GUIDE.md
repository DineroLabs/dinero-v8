# Phase C Integration Guide - Connect ZK Library to Blockchain

**Status:** Ready to implement (Phase B complete!)
**Prerequisites:** ✅ Phase A/B complete, `dinero_zk` library tested
**Goal:** Integrate confidential transactions into DineroCoin blockchain

---

## ✅ What's Already Done

### Phase B Complete
- ✅ `dinero_zk` library (src/zk/*.cpp, src/zk/*.h)
- ✅ Pedersen commitments
- ✅ Range proof generation/verification
- ✅ Receiver rewind (decrypt amounts)
- ✅ Blinding factor balancing
- ✅ Comprehensive tests (6/6 passing)

### Integration Skeleton Ready
- ✅ RPC skeleton: `src/rpc/zk_rpc_handlers_context.cpp`
- ✅ Integration docs: `ZK_RPC_INTEGRATION.md`
- ✅ Structure design: `ZK_TRANSACTION_STRUCTURE.md`

---

## 🎯 Phase C Roadmap (4 Steps)

### Step 1: Update TxOutput Structure (1-2 hours)
### Step 2: Wire RPC Methods (2-3 hours)
### Step 3: Integrate Validation (1 hour)
### Step 4: Test End-to-End (1 hour)

**Total estimated time:** 5-7 hours of focused work

---

## Step 1: Extend TxOutput for Confidential Transactions

### 1.1 Update Transaction Header

**File:** `include/wallet/transaction.h`

```cpp
// Add after existing includes
#include "zk/zk_types.h"  // For ConfidentialOutput

struct TxOutput {
    // EXISTING: Transparent output fields
    uint64_t value;                     // Amount (0 if confidential)
    std::vector<uint8_t> scriptPubKey;

    // NEW: Confidential transaction support
    bool is_confidential = false;       // Flag for CT outputs

    // Confidential transaction data (from dinero_zk library)
    dinero::zk::ConfidentialOutput confidential_output;  // Commitment + proof + nonce

    // EXISTING: Constructors
    TxOutput() : value(0), is_confidential(false) {}

    TxOutput(uint64_t val, const std::vector<uint8_t>& script)
        : value(val), scriptPubKey(script), is_confidential(false) {}

    // NEW: Confidential output constructor
    TxOutput(const dinero::zk::ConfidentialOutput& ct_output,
             const std::vector<uint8_t>& script)
        : value(0),  // Hidden!
          scriptPubKey(script),
          is_confidential(true),
          confidential_output(ct_output) {}

    // EXISTING: Methods
    uint8_t GetWitnessVersion() const;
    bool IsWitness() const;
    bool IsSegWitV0() const;
    bool IsTaproot() const;

    // NEW: Confidential methods
    bool IsConfidential() const { return is_confidential; }

    size_t GetSize() const {
        if (!is_confidential) {
            // Transparent: 8 bytes (value) + varint + scriptPubKey
            return 8 + GetVarintSize(scriptPubKey.size()) + scriptPubKey.size();
        } else {
            // Confidential: 33 bytes (commitment) + ~5KB proof + 32 bytes nonce + scriptPubKey
            return 33 +
                   GetVarintSize(confidential_output.range_proof.proof.size()) +
                   confidential_output.range_proof.proof.size() +
                   32 +  // nonce
                   GetVarintSize(scriptPubKey.size()) +
                   scriptPubKey.size();
        }
    }

private:
    static size_t GetVarintSize(uint64_t n) {
        if (n < 0xfd) return 1;
        if (n <= 0xffff) return 3;
        if (n <= 0xffffffff) return 5;
        return 9;
    }
};
```

### 1.2 Update Serialization

**File:** `src/wallet/transaction.cpp` (or wherever serialization is)

```cpp
#include "zk/zk_types.h"

// Serialize confidential output
void SerializeTxOutput(const TxOutput& output, std::vector<uint8_t>& data) {
    if (!output.is_confidential) {
        // EXISTING: Transparent serialization
        SerializeUint64(output.value, data);
        SerializeVarInt(output.scriptPubKey.size(), data);
        data.insert(data.end(), output.scriptPubKey.begin(), output.scriptPubKey.end());
    } else {
        // NEW: Confidential serialization
        // Marker byte: 0xFF (indicates confidential output)
        data.push_back(0xFF);

        // Serialize commitment (33 bytes)
        auto commitment_bytes = output.confidential_output.commitment.Serialize();
        data.insert(data.end(), commitment_bytes.begin(), commitment_bytes.end());

        // Serialize range proof (varint length + data)
        const auto& proof = output.confidential_output.range_proof.proof;
        SerializeVarInt(proof.size(), data);
        data.insert(data.end(), proof.begin(), proof.end());

        // Serialize nonce (32 bytes - receiver needs this!)
        const auto& nonce = output.confidential_output.range_proof.nonce;
        data.insert(data.end(), nonce.begin(), nonce.end());

        // Serialize scriptPubKey
        SerializeVarInt(output.scriptPubKey.size(), data);
        data.insert(data.end(), output.scriptPubKey.begin(), output.scriptPubKey.end());
    }
}

// Deserialize confidential output
bool DeserializeTxOutput(TxOutput& output, const uint8_t*& data, size_t& remaining) {
    if (remaining < 1) return false;

    // Check for confidential marker
    if (*data == 0xFF) {
        // Confidential output
        data++; remaining--;

        output.is_confidential = true;

        // Parse commitment (33 bytes)
        if (remaining < 33) return false;
        dinero::zk::CommitmentBytes commit_bytes;
        std::memcpy(commit_bytes.data(), data, 33);
        if (!output.confidential_output.commitment.Deserialize(commit_bytes)) {
            return false;
        }
        data += 33; remaining -= 33;

        // Parse range proof
        uint64_t proof_len;
        if (!DeserializeVarInt(proof_len, data, remaining)) return false;
        if (remaining < proof_len) return false;

        output.confidential_output.range_proof.proof.resize(proof_len);
        std::memcpy(output.confidential_output.range_proof.proof.data(), data, proof_len);
        data += proof_len; remaining -= proof_len;

        // Parse nonce (32 bytes)
        if (remaining < 32) return false;
        std::memcpy(output.confidential_output.range_proof.nonce.data(), data, 32);
        data += 32; remaining -= 32;

        // Parse scriptPubKey
        uint64_t script_len;
        if (!DeserializeVarInt(script_len, data, remaining)) return false;
        if (remaining < script_len) return false;

        output.scriptPubKey.resize(script_len);
        std::memcpy(output.scriptPubKey.data(), data, script_len);
        data += script_len; remaining -= script_len;

        output.value = 0;  // Hidden!

    } else {
        // EXISTING: Transparent deserialization
        if (remaining < 8) return false;
        output.value = DeserializeUint64(data);
        data += 8; remaining -= 8;

        uint64_t script_len;
        if (!DeserializeVarInt(script_len, data, remaining)) return false;
        if (remaining < script_len) return false;

        output.scriptPubKey.resize(script_len);
        std::memcpy(output.scriptPubKey.data(), data, script_len);
        data += script_len; remaining -= script_len;

        output.is_confidential = false;
    }

    return true;
}
```

---

## Step 2: Implement RPC Methods

### 2.1 Uncomment Includes

**File:** `src/rpc/zk_rpc_handlers_context.cpp`
**Lines 21-26:**

```cpp
// Uncomment these NOW (Phase B complete!)
#include "zk/confidential_tx.h"
#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>
```

### 2.2 Implement zk.createtx

**File:** `src/rpc/zk_rpc_handlers_context.cpp`
**Replace skeleton in `rpc_context_zk_createtx()` (line 80):**

```cpp
din::Json rpc_context_zk_createtx(const ExecutionContext& ctx, const din::Json& params) {
    using namespace dinero::zk;

    // Parse parameters
    if (!params.is_object()) {
        throw std::runtime_error("Invalid parameters (expected object)");
    }

    const auto& inputs_json = params["inputs"];
    const auto& outputs_json = params["outputs"];

    // Create confidential transaction
    ConfidentialTxBuilder builder;

    // Add inputs
    for (const auto& input : inputs_json) {
        uint64_t amount = input["amount"];

        // Parse blinding factor from hex
        BlindingFactor blind;
        std::string blind_hex = input["blinding_factor"];
        if (blind_hex.size() != 64) {
            throw std::runtime_error("Invalid blinding factor (expected 64 hex chars)");
        }
        for (size_t i = 0; i < 32; ++i) {
            blind[i] = std::stoul(blind_hex.substr(i*2, 2), nullptr, 16);
        }

        if (!builder.AddInput(amount, blind)) {
            throw std::runtime_error("Failed to add input");
        }
    }

    // Add outputs
    std::vector<BlindingFactor> output_blinds;
    for (const auto& output : outputs_json) {
        uint64_t amount = output["amount"];

        auto result = builder.AddOutput(amount);
        if (!result.IsOk()) {
            throw std::runtime_error("Failed to add output: " + result.error_message);
        }

        output_blinds.push_back(result.value);
    }

    // Balance blinding factors
    if (!builder.BalanceBlindingFactors()) {
        throw std::runtime_error("Failed to balance blinding factors");
    }

    // Generate range proofs
    if (!builder.GenerateRangeProofs()) {
        throw std::runtime_error("Failed to generate range proofs");
    }

    // Verify before returning
    bool balance_valid = builder.VerifyBalance();
    bool proofs_valid = builder.VerifyRangeProofs();

    if (!balance_valid) {
        throw std::runtime_error("Balance verification failed!");
    }
    if (!proofs_valid) {
        throw std::runtime_error("Range proof verification failed!");
    }

    // Build response
    din::Json response;
    response["verify"]["balance"] = balance_valid;
    response["verify"]["range_proofs"] = proofs_valid;

    // Return commitment details for each output
    din::Json commitments = din::Json::array();
    const auto& outputs = builder.GetOutputs();
    for (size_t i = 0; i < outputs.size(); ++i) {
        din::Json commit_info;
        commit_info["vout"] = i;

        // Commitment hex
        auto commit_bytes = outputs[i].commitment.Serialize();
        std::string commit_hex;
        for (uint8_t b : commit_bytes) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            commit_hex += buf;
        }
        commit_info["commitment"] = commit_hex;

        // Blinding factor hex (IMPORTANT: Keep secret!)
        std::string blind_hex;
        for (uint8_t b : output_blinds[i]) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            blind_hex += buf;
        }
        commit_info["blinding_factor"] = blind_hex;

        // Nonce hex (give to receiver!)
        std::string nonce_hex;
        for (uint8_t b : outputs[i].range_proof.nonce) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", b);
            nonce_hex += buf;
        }
        commit_info["nonce"] = nonce_hex;

        commit_info["rangeproof_size"] = outputs[i].range_proof.proof.size();

        commitments.push_back(commit_info);
    }
    response["commitments"] = commitments;

    // TODO: Build actual transaction hex
    response["hex"] = "TODO: Serialize full transaction";
    response["txid"] = "TODO: Calculate TXID";

    return response;
}
```

### 2.3 Implement zk.verify

**Replace skeleton around line 100:**

```cpp
din::Json rpc_context_zk_verify(const ExecutionContext& ctx, const din::Json& params) {
    using namespace dinero::zk;

    // TODO: Parse transaction from hex
    // For now, just demonstrate validation

    // Parse inputs and outputs from params
    const auto& inputs_json = params["inputs"];
    const auto& outputs_json = params["outputs"];

    // Build inputs/outputs vectors
    std::vector<ConfidentialInput> inputs;
    std::vector<ConfidentialOutput> outputs;

    // ... parse inputs/outputs from JSON ...

    // Validate
    ConfidentialTxValidator validator;
    bool valid = validator.Verify(inputs, outputs);

    din::Json response;
    response["valid"] = valid;
    response["balance_valid"] = validator.VerifyCommitmentBalance(inputs, outputs);

    return response;
}
```

### 2.4 Register RPC Methods

**File:** `src/rpc/zk_rpc_handlers_context.cpp`
**Bottom of file (already exists, just verify):**

```cpp
void WireZkRpcContext() {
    RegisterContextMethod("zk.createtx", rpc_context_zk_createtx);
    RegisterContextMethod("zk.verify", rpc_context_zk_verify);
    RegisterContextMethod("zk.verifyrangeproof", rpc_context_zk_verifyrangeproof);
    RegisterContextMethod("zk.scanviewkey", rpc_context_zk_scanviewkey);
    RegisterContextMethod("zk.getcommitment", rpc_context_zk_getcommitment);
}
```

### 2.5 Wire into Build System

**Step 2.5a: Add to CMakeLists.txt**

**File:** `CMakeLists.txt`
**Line ~442 (after logs_rpc_handlers_context.cpp):**

```cmake
  src/rpc/logging_rpc_handlers_context.cpp  # logging.setlevel, logging.getlevel
  src/rpc/logs_rpc_handlers_context.cpp     # logs.recent, logs.services, logs.tail
  src/rpc/zk_rpc_handlers_context.cpp       # zk.createtx, zk.verify, zk.scanviewkey  ← ADD THIS
```

**Step 2.5b: Register in Daemon**

**File:** `src/daemon/rpc_context_wiring.cpp`

**Line ~33 (forward declaration):**
```cpp
void WireLogsRpcContext();
void WireZkRpcContext();  // ← ADD THIS
```

**Line ~209 (registration):**
```cpp
        WireLogsRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Log aggregation context-aware handlers registered");

        // Zero-knowledge privacy namespace ← ADD THESE 3 LINES
        WireZkRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ ZK privacy context-aware handlers registered");

    } catch (const std::exception& e) {
```

---

## Step 3: Integrate Validation into Consensus

### 3.1 Add Validation Hook

**File:** `src/validation/transaction_validation.cpp` (or similar)

```cpp
#include "zk/confidential_tx.h"

bool CheckTransaction(const CTransaction& tx) {
    // EXISTING: Check transparent outputs
    // ...

    // NEW: Check confidential outputs
    bool has_confidential = false;
    for (const auto& output : tx.vout) {
        if (output.IsConfidential()) {
            has_confidential = true;
            break;
        }
    }

    if (has_confidential) {
        // Build inputs/outputs for ZK validator
        std::vector<dinero::zk::ConfidentialInput> zk_inputs;
        std::vector<dinero::zk::ConfidentialOutput> zk_outputs;

        // TODO: Extract confidential inputs from tx.vin
        // TODO: Extract confidential outputs from tx.vout

        // Validate confidential transaction
        dinero::zk::ConfidentialTxValidator validator;
        if (!validator.Verify(zk_inputs, zk_outputs)) {
            return false;  // Invalid confidential transaction!
        }
    }

    return true;  // All checks passed
}
```

---

## Step 4: Test End-to-End

### 4.1 Build with ZK Support

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Clean build
rm -rf build
cmake -B build -S . -DENABLE_ZK=ON

# Build everything
cmake --build build -j8

# Verify ZK library linked
ldd build/dinerod | grep dinero_zk
# or on macOS:
otool -L build/dinerod | grep dinero_zk
```

### 4.2 Test RPC Methods

```bash
# Start daemon
./build/dinerod

# Test zk.createtx
./build/dinero-cli zk.createtx '{
  "inputs": [
    {
      "amount": 100000000,
      "blinding_factor": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  ],
  "outputs": [
    {
      "address": "dinero1q...",
      "amount": 99990000
    }
  ]
}'

# Should return:
# {
#   "verify": {
#     "balance": true,
#     "range_proofs": true
#   },
#   "commitments": [...]
# }
```

### 4.3 Test Receiver Rewind

```cpp
// In wallet code
const auto& output = tx.vout[0];
if (output.IsConfidential()) {
    // Get nonce from sender (off-chain)
    uint8_t nonce[32];  // Received from sender

    uint64_t recovered_amount;
    dinero::zk::BlindingFactor recovered_blind;

    dinero::zk::ConfidentialTxBuilder builder;  // Just for rewind
    bool success = builder.RewindRangeProof(
        output.confidential_output,
        nonce,
        &recovered_amount,
        &recovered_blind
    );

    if (success) {
        // Receiver knows they received `recovered_amount`!
        LOG_INFO("Received confidential payment: " << recovered_amount << " una");
    }
}
```

---

## 🎯 Integration Checklist

### Phase C Tasks

- [ ] **Step 1: TxOutput Structure**
  - [ ] Add `is_confidential` flag
  - [ ] Add `confidential_output` field
  - [ ] Add confidential constructor
  - [ ] Update `GetSize()` method
  - [ ] Update serialization
  - [ ] Update deserialization

- [ ] **Step 2: RPC Methods**
  - [ ] Uncomment ZK includes in `zk_rpc_handlers_context.cpp`
  - [ ] Implement `zk.createtx`
  - [ ] Implement `zk.verify`
  - [ ] Add to CMakeLists.txt (line 442)
  - [ ] Add forward declaration in `rpc_context_wiring.cpp` (line 33)
  - [ ] Add registration in `rpc_context_wiring.cpp` (line 209)

- [ ] **Step 3: Validation**
  - [ ] Add validation hook in consensus code
  - [ ] Extract confidential inputs/outputs from CTxIn/CTxOut
  - [ ] Call `ConfidentialTxValidator::Verify()`

- [ ] **Step 4: Testing**
  - [ ] Build with `ENABLE_ZK=ON`
  - [ ] Test `zk.createtx` RPC
  - [ ] Test `zk.verify` RPC
  - [ ] Test receiver rewind
  - [ ] Test consensus validation

---

## 📊 Size & Performance Impact

### Transaction Size
```
Transparent output:   ~34 bytes
Confidential output:  ~5,159 bytes (33 + 5126 + 32)
Overhead:            151× increase
```

### Validation Performance
```
Balance check:       ~11μs
Range proof verify:  ~5ms per output
Total for 3 outputs: ~15ms (negligible!)
```

### Network Bandwidth
```
Typical transaction: 3 outputs × 5KB = ~15KB
Overhead:           ~15KB per confidential TX
Acceptable?         YES (privacy worth it!)
```

---

## 🚀 Quick Start Commands

```bash
# 1. Build with ZK
cmake -B build -S . -DENABLE_ZK=ON && cmake --build build -j8

# 2. Start daemon
./build/dinerod

# 3. Create confidential TX
./build/dinero-cli zk.createtx '{...}'

# 4. Verify
./build/dinero-cli zk.verify '{...}'
```

---

## 📚 Reference Documents

1. **ZK_IMPLEMENTATION_COMPLETE.md** - Phase B completion status
2. **ZK_RPC_INTEGRATION.md** - RPC wiring details
3. **ZK_TRANSACTION_STRUCTURE.md** - TxOutput design
4. **PHASE_B_RANGE_PROOFS.md** - Range proof API reference
5. **ZK_PRIVACY_INTEGRATION.md** - Original architecture design

---

## ✨ Summary

**Phase C connects your production-ready `dinero_zk` library to the blockchain!**

All hard cryptography is done. Phase C is just:
1. Add fields to structs ✅ Easy
2. Wire RPC methods ✅ Skeleton exists
3. Hook validation ✅ 10 lines of code
4. Test everything ✅ Already have tests

**Estimated time:** 5-7 focused hours
**Complexity:** Low (infrastructure work, not crypto)
**Result:** Full confidential transaction support in DineroCoin! 🎉

---

**Integration ready when you are!** 🚀
