# Phase C.4: Covenant RPC Endpoints - Implementation Plan

**Date**: 2025-12-27
**Status**: Planning
**Foundation**: Built on Phase C.3 (covenant construction helpers)

---

## 🎯 Phase C.4 Objectives

**Goal**: Expose covenant construction helpers via RPC interface

**Scope**: RPC layer ONLY, no new wallet logic
- ✅ Expose CTV template building
- ✅ Expose CSFS delegation signing
- ✅ Expose covenant script creation
- ✅ Expose fee estimation
- ❌ NO validation logic (construction only)

**Non-Negotiable Rule**:
```
RPC Layer → Wallet Construction → (Consensus Validation - separate)
   (API)          (helpers)              (authority)
```

---

## 🧱 Architectural Constraints

| Rule | Status |
|------|--------|
| RPC methods expose wallet construction helpers | ✅ |
| RPC methods NEVER validate covenants | ✅ |
| Use ExecutionContext pattern (no globals) | ✅ |
| Follow existing wallet RPC patterns | ✅ |
| Return JSON with error handling | ✅ |

---

## 🎨 RPC Method Design

### Pattern (From methods_wallet_confidential.cpp)

```cpp
din::Json rpc_context_wallet_methodname(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    din::Json result;

    // 1. Validate context
    if (!ctx.daemon || !ctx.daemon->wallet) {
        result["error"] = "Wallet service not available";
        return result;
    }

    // 2. Get wallet service
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(
        ctx.daemon->wallet
    );

    // 3. Call covenant builder helper
    try {
        auto template_result = dinero::wallet::buildCTVTemplate(...);
        result["template_hash"] = hexEncode(template_result.template_hash);
        result["success"] = true;
    } catch (const std::exception& e) {
        result["error"] = std::string("Failed: ") + e.what();
    }

    return result;
}
```

### Registration Pattern

```cpp
void register_context_wallet_covenant_methods() {
    g_rpcRegistry.registerHandler(
        "wallet.createctvtemplate",
        rpc_context_wallet_createctvtemplate,
        RegisterMode::Overwrite,
        "context-aware"
    );
}
```

---

## 📋 RPC Methods to Implement

### 1. wallet.createctvtemplate

**Purpose**: Build a CTV template from outputs

**Parameters**:
```json
{
  "outputs": [
    {
      "address": "tb1q...",
      "value": 50000
    },
    {
      "address": "tb1q...",
      "value": 40000
    }
  ],
  "locktime": 0,
  "version": 2
}
```

**Returns**:
```json
{
  "template_hash": "abcd1234...",
  "outputs": [...],
  "locktime": 0,
  "version": 2,
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::buildCTVTemplate()`

---

### 2. wallet.createctvscript

**Purpose**: Create a CTV-locked scriptPubKey

**Parameters**:
```json
{
  "template_hash": "abcd1234...",
  "use_taproot": false
}
```

**Returns**:
```json
{
  "script": "0020...",
  "script_hex": "0020abcd1234...",
  "type": "p2wsh_ctv",
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::createCTVScript()`

---

### 3. wallet.buildctvspending

**Purpose**: Build a transaction that spends a CTV output

**Parameters**:
```json
{
  "template_hash": "abcd1234...",
  "funding_utxo": {
    "txid": "1111...",
    "vout": 0,
    "value": 100000,
    "height": 100
  },
  "outputs": [...]
}
```

**Returns**:
```json
{
  "tx": {...},
  "tx_hex": "0200000001...",
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::buildCTVSpendingTx()`

---

### 4. wallet.createcsfsdelegation

**Purpose**: Create an unsigned CSFS delegation

**Parameters**:
```json
{
  "pubkey": "abcd1234...",
  "message": "delegation message",
  "purpose": "oracle"
}
```

**Returns**:
```json
{
  "pubkey": "abcd1234...",
  "message": "delegation message",
  "message_hex": "64656c65...",
  "purpose": "oracle",
  "is_signed": false,
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::createCSFSDelegation()`

---

### 5. wallet.signcsfs

**Purpose**: Sign a CSFS delegation with a private key

**Parameters**:
```json
{
  "delegation": {
    "pubkey": "abcd1234...",
    "message": "delegation message",
    "purpose": "oracle"
  },
  "privkey": "secret1234..."
}
```

**Returns**:
```json
{
  "pubkey": "abcd1234...",
  "message": "delegation message",
  "signature": "abcd1234...",
  "is_signed": true,
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::signCSFSDelegation()`

**Security Note**: Private key handling - consider using wallet's internal keys

---

### 6. wallet.createcsfsscript

**Purpose**: Create a CSFS-locked scriptPubKey

**Parameters**:
```json
{
  "pubkey": "abcd1234...",
  "message": "delegation message",
  "continuation_script": ""
}
```

**Returns**:
```json
{
  "script": "20abcd...",
  "script_hex": "20abcd1234...",
  "type": "tapscript_csfs",
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::createCSFSScript()`

---

### 7. wallet.estimatecovenantfee

**Purpose**: Estimate fees for covenant transactions

**Parameters**:
```json
{
  "covenant_type": "ctv",
  "template_size": 32
}
```

**Returns**:
```json
{
  "covenant_type": "ctv",
  "witness_size": 36,
  "witness_vbytes": 36,
  "success": true
}
```

**Implementation**: Calls `dinero::wallet::estimateCovenantWitnessSize()`

---

## 🗂️ File Structure

### New File

**src/rpc/methods_wallet_covenant.cpp**
- All covenant RPC method implementations
- Registration function
- Error handling
- JSON parameter parsing

**include/rpc/methods_wallet_covenant.h** (if needed)
- Function declarations
- Registration function prototype

---

## Implementation Order

### Stage 1: Core RPC Methods (1-2 days)
1. Create `src/rpc/methods_wallet_covenant.cpp`
2. Implement `wallet.createctvtemplate`
3. Implement `wallet.createctvscript`
4. Implement `wallet.buildctvspending`
5. Add registration function
6. Test with manual RPC calls

### Stage 2: CSFS RPC Methods (1-2 days)
1. Implement `wallet.createcsfsdelegation`
2. Implement `wallet.signcsfs`
3. Implement `wallet.createcsfsscript`
4. Test with manual RPC calls

### Stage 3: Utility Methods (1 day)
1. Implement `wallet.estimatecovenantfee`
2. Add comprehensive error handling
3. Add parameter validation

### Stage 4: Testing (1-2 days)
1. Create `tests/rpc/test_rpc_wallet_covenant.cpp`
2. Test all RPC methods
3. Test error cases
4. Test integration with covenant builders

### Stage 5: Documentation (1 day)
1. Add RPC method documentation
2. Create usage examples
3. Document JSON schemas
4. Sign off Phase C.4

**Total Duration**: 5-8 days

---

## Critical Files

**New Files**:
- `src/rpc/methods_wallet_covenant.cpp` - RPC method implementations
- `tests/rpc/test_rpc_wallet_covenant.cpp` - RPC tests

**Modified Files** (potentially):
- RPC registration caller (wherever `register_context_wallet_covenant_methods()` is called)

---

## Error Handling

### Standard Error Response

```json
{
  "error": "Error message here",
  "error_code": "COVENANT_BUILD_FAILED",
  "details": "Additional context"
}
```

### Common Errors

- Wallet service not available
- Invalid parameters
- Construction failed (e.g., invalid pubkey)
- Signing failed (e.g., wrong private key)

---

## Parameter Validation

### Required Validations

1. **Address validation**: Bech32 decoding
2. **Value validation**: Non-negative, within limits
3. **Hex validation**: Valid hex strings for hashes/keys
4. **Length validation**: 32 bytes for hashes, etc.

### Validation Pattern

```cpp
// Validate hex parameter
if (!isValidHex(params["template_hash"].get<std::string>())) {
    result["error"] = "Invalid hex in template_hash";
    return result;
}

// Validate array parameter
if (!params.contains("outputs") || !params["outputs"].is_array()) {
    result["error"] = "outputs must be an array";
    return result;
}
```

---

## JSON Schema Examples

### wallet.createctvtemplate Request

```json
{
  "method": "wallet.createctvtemplate",
  "params": {
    "outputs": [
      {"address": "tb1q...", "value": 50000},
      {"address": "tb1q...", "value": 40000}
    ],
    "locktime": 0,
    "version": 2
  }
}
```

### wallet.createctvtemplate Response (Success)

```json
{
  "template_hash": "abcd1234ef5678...",
  "outputs": [
    {
      "value": 50000,
      "script_pubkey": "0014...",
      "address": "tb1q..."
    },
    {
      "value": 40000,
      "script_pubkey": "0014...",
      "address": "tb1q..."
    }
  ],
  "locktime": 0,
  "version": 2,
  "success": true,
  "rpc_schema": "din.wallet.covenant.v1"
}
```

### wallet.createctvtemplate Response (Error)

```json
{
  "error": "Invalid Bech32 address: tb1qinvalid",
  "success": false
}
```

---

## Security Considerations

### Private Key Handling

**wallet.signcsfs** accepts private keys - security concerns:
1. Private key transmitted over RPC (if remote)
2. Private key in RPC logs (if enabled)
3. Private key in memory

**Mitigation Options**:
- Add warning in documentation
- Consider alternative: `wallet.signcsfs_internal` using wallet's own keys
- Future: Hardware wallet signing support

### Input Validation

- All hex strings validated before parsing
- All addresses validated before decoding
- All numeric values range-checked
- All array sizes limited (DoS protection)

---

## Testing Strategy

### Unit Tests

Test each RPC method individually:
1. Valid parameters → Success
2. Invalid parameters → Error
3. Missing parameters → Error
4. Edge cases (empty arrays, max values, etc.)

### Integration Tests

Test full covenant flow via RPC:
1. Create CTV template via RPC
2. Create CTV script via RPC
3. Build spending tx via RPC
4. Verify construction matches expected

### RPC Test Framework

Use existing RPC test patterns:
```cpp
ExecutionContext ctx;
ctx.daemon = /* mock daemon */;

din::Json params;
params["outputs"] = /* ... */;

auto result = rpc_context_wallet_createctvtemplate(ctx, params);
assert(result.contains("template_hash"));
assert(result["success"].get<bool>() == true);
```

---

## Success Criteria

Phase C.4 is complete when:

- ✅ All 7 RPC methods implemented
- ✅ All methods follow ExecutionContext pattern
- ✅ All methods registered with g_rpcRegistry
- ✅ Parameter validation comprehensive
- ✅ Error handling robust
- ✅ RPC tests passing
- ✅ Integration with covenant builders working
- ✅ No validation logic in RPC layer
- ✅ Documentation complete

---

## Boundary Enforcement

### Critical Boundary Rule

```
RPC ONLY exposes construction helpers
RPC NEVER validates covenant rules
```

### Allowed in RPC

✅ **Parameter parsing and validation**:
- Validate hex strings
- Validate addresses
- Validate numeric ranges

✅ **Call covenant builders**:
- buildCTVTemplate()
- createCTVScript()
- buildCTVSpendingTx()
- signCSFSDelegation()

### Forbidden in RPC

❌ **Covenant validation**:
- NO calls to consensus::VerifyCTV()
- NO calls to consensus::VerifySignatureFromStack()
- NO checking if covenant rules are satisfied
- NO "valid/invalid" returns based on covenant checks

---

## Next Immediate Action

**Start with RPC implementation**:

1. Create `src/rpc/methods_wallet_covenant.cpp`
2. Add includes and registration function
3. Implement `wallet.createctvtemplate` first
4. Test manually via RPC
5. Iterate to remaining methods

---

## Phase Dependencies

**Requires**:
- ✅ Phase C.1 (covenant consensus) - COMPLETE
- ✅ Phase C.2 (covenant mempool policy) - COMPLETE
- ✅ Phase C.3 (covenant construction helpers) - COMPLETE

**Enables**:
- Advanced covenant applications (vaults, channels)
- User-facing covenant tools
- Lightning covenant integration
- Covenant script development

---

**Plan Status**: ✅ Ready for implementation
