# Phase 4: RPC/API Layer Completion & Testing

**Status:** Planning → Implementation
**Prerequisites:** ✅ Phase 3 Complete (Transaction Spending & Fee Logic)
**Goal:** Complete, test, and document the RPC API for transaction spending
**Timeline:** 2-3 weeks

---

## Overview

Phase 4 completes the RPC/API layer that exposes Phase 3's transaction spending capabilities to users and applications. This phase focuses on testing, documenting, and enhancing the existing RPC infrastructure to ensure production readiness.

**Key Discovery:** The RPC infrastructure is already implemented! Our task is to:
1. Verify it works correctly with Phase 3 components
2. Add comprehensive testing
3. Document the API
4. Fill any gaps

---

## Current State Assessment

### What Already Exists ✅

**RPC Framework:**
- ✅ Context-aware RPC handlers (`ExecutionContext&`)
- ✅ RPC registry and routing
- ✅ Service injection (no global variables)
- ✅ JSON request/response handling

**Wallet RPC Methods:**
- ✅ `wallet.getbalance` - Get wallet balance
- ✅ `wallet.sendtoaddress` - Send coins to address
- ✅ `wallet.getnewaddress` - Generate new receiving address
- ✅ `wallet.listunspent` - List unspent outputs
- ✅ `wallet.listtransactions` - List wallet transactions
- ✅ `wallet.getaddressinfo` - Get address details
- ✅ **39 total wallet RPC methods**

**Integration with Phase 3:**
The existing `wallet.sendtoaddress` already uses:
- ✅ `UnsignedTxBuilder` - Transaction construction
- ✅ `TransactionSigner` - BIP143 signing
- ✅ Mempool submission
- ✅ Fee estimation
- ✅ Coin selection (greedy)

**What This Means:**
The integration work is DONE! We need to verify and test it.

---

## Phase 4 Implementation Plan

### Week 1: RPC Testing & Verification

#### Task 1.1: Create RPC Integration Test Suite

**File:** `tests/rpc/test_wallet_rpc_integration.cpp`

**Test Cases:**
1. `wallet.getbalance` - Verify balance accuracy
2. `wallet.getnewaddress` - Address generation
3. `wallet.sendtoaddress` - End-to-end spending
4. `wallet.listunspent` - UTXO enumeration
5. `wallet.listtransactions` - Transaction history
6. RPC error handling (insufficient funds, invalid address, etc.)

**Example Test:**
```cpp
TEST(WalletRPC, SendToAddress) {
    // Setup: Mine blocks for spendable coins
    RPCClient rpc("127.0.0.1", 20998);

    // Mine 101 blocks (coinbase maturity)
    std::string address = rpc.call("wallet.getnewaddress").get("address");
    for (int i = 0; i < 101; i++) {
        rpc.call("generatetoaddress", {1, address});
    }

    // Verify balance
    auto balance = rpc.call("wallet.getbalance");
    EXPECT_GT(balance.get("confirmed").as<double>(), 0);

    // Send transaction
    std::string recipient = rpc.call("wallet.getnewaddress").get("address");
    auto result = rpc.call("wallet.sendtoaddress", {recipient, 10.0});

    EXPECT_TRUE(result.has("txid"));
    std::string txid = result.get("txid").as<std::string>();
    EXPECT_EQ(txid.length(), 64);  // Valid txid

    // Verify transaction in mempool
    auto mempool = rpc.call("getrawmempool");
    EXPECT_TRUE(mempool.contains(txid));

    // Mine confirmation block
    rpc.call("generatetoaddress", {1, address});

    // Verify balance updated
    auto new_balance = rpc.call("wallet.getbalance");
    EXPECT_LT(new_balance.get("confirmed").as<double>(),
              balance.get("confirmed").as<double>());
}
```

#### Task 1.2: Test Phase 3 Component Integration

**Goal:** Verify that RPC methods correctly use Phase 3 components

**Test Cases:**
```cpp
TEST(RPC_Phase3_Integration, CoinbaseMaturity) {
    // Mine 1 block
    auto address = rpc.call("wallet.getnewaddress").get("address");
    rpc.call("generatetoaddress", {1, address});

    // Try to spend immediately (should fail - immature)
    auto result = rpc.call("wallet.sendtoaddress", {address, 10.0});
    EXPECT_TRUE(result.has("error"));
    EXPECT_THAT(result.get("error").as<std::string>(),
                HasSubstr("No confirmed UTXOs"));

    // Mine 100 more blocks
    rpc.call("generatetoaddress", {100, address});

    // Now should succeed (mature coinbase)
    auto result2 = rpc.call("wallet.sendtoaddress", {address, 10.0});
    EXPECT_TRUE(result2.has("txid"));
}

TEST(RPC_Phase3_Integration, CoinSelection) {
    // Test that greedy coin selection works
    // Mine multiple blocks to create many UTXOs
    // Send transaction and verify it selected optimal UTXOs
}

TEST(RPC_Phase3_Integration, BIP143Signing) {
    // Send transaction
    // Retrieve raw transaction
    // Verify witness data structure
    // Verify signature is valid BIP143
}

TEST(RPC_Phase3_Integration, FeeEstimation) {
    // Send transaction with auto fee estimation
    // Verify fee is reasonable
    // Send with manual fee rate
    // Verify fee matches expectation
}
```

#### Task 1.3: RPC Error Handling Tests

**Test Cases:**
```cpp
TEST(WalletRPC, ErrorHandling) {
    RPCClient rpc;

    // Test 1: Send without sufficient funds
    auto result1 = rpc.call("wallet.sendtoaddress",
                           {"bc1q...", 1000000.0});
    EXPECT_TRUE(result1.has("error"));
    EXPECT_THAT(result1.get("error"), HasSubstr("Insufficient funds"));

    // Test 2: Invalid address format
    auto result2 = rpc.call("wallet.sendtoaddress",
                           {"invalid_address", 10.0});
    EXPECT_TRUE(result2.has("error"));
    EXPECT_THAT(result2.get("error"), HasSubstr("Invalid address"));

    // Test 3: Negative amount
    auto result3 = rpc.call("wallet.sendtoaddress",
                           {"bc1q...", -10.0});
    EXPECT_TRUE(result3.has("error"));
    EXPECT_THAT(result3.get("error"), HasSubstr("must be positive"));

    // Test 4: Locked wallet
    rpc.call("wallet.lock");
    auto result4 = rpc.call("wallet.sendtoaddress",
                           {"bc1q...", 10.0});
    EXPECT_TRUE(result4.has("error"));
    EXPECT_THAT(result4.get("error"), HasSubstr("locked"));
}
```

---

### Week 2: RPC API Documentation

#### Task 2.1: Create RPC API Reference

**File:** `docs/RPC_API_REFERENCE.md`

**Sections:**
1. **Overview** - RPC system architecture
2. **Authentication** - How to connect and authenticate
3. **Request Format** - JSON-RPC request structure
4. **Response Format** - Success and error responses
5. **Wallet Methods** - Complete API reference
6. **Blockchain Methods** - Query blockchain state
7. **Network Methods** - P2P network information
8. **Mining Methods** - Block generation
9. **Error Codes** - Standard error codes

**Example Entry:**
```markdown
## wallet.sendtoaddress

Send an amount to a given address.

**Syntax:**
```
wallet.sendtoaddress <address> <amount> [fee_rate] [comment]
```

**Parameters:**
1. `address` (string, required) - The recipient address (Bech32 format)
2. `amount` (numeric, required) - The amount in DIN to send
3. `fee_rate` (numeric, optional) - Fee rate in sat/vB (auto-estimated if omitted)
4. `comment` (string, optional) - Transaction comment (not broadcast)

**Returns:**
```json
{
  "txid": "abc123...",
  "fee": 0.00001,
  "fee_rate": 1.0,
  "vsize": 141,
  "change_amount": 39.99989,
  "inputs_count": 1,
  "outputs_count": 2
}
```

**Errors:**
- `"No active wallet"` - Wallet not loaded
- `"Wallet is locked"` - Wallet locked, use wallet.unlock
- `"Invalid address format"` - Address validation failed
- `"Insufficient funds"` - Not enough confirmed balance
- `"No confirmed UTXOs available"` - All coins immature

**Examples:**
```bash
# Send 10 DIN with auto fee estimation
dinero-cli wallet.sendtoaddress "bc1q..." 10.0

# Send with manual fee rate (5 sat/vB)
dinero-cli wallet.sendtoaddress "bc1q..." 10.0 5.0

# Send with comment
dinero-cli wallet.sendtoaddress "bc1q..." 10.0 1.0 "Payment for services"
```

**Related:**
- `wallet.getbalance` - Check available balance
- `wallet.listtransactions` - View transaction history
- `wallet.listunspent` - List available UTXOs
```

#### Task 2.2: Create RPC Quick Start Guide

**File:** `docs/RPC_QUICK_START.md`

**Contents:**
1. **Installation** - Setting up RPC access
2. **First Transaction** - Step-by-step walkthrough
3. **Common Use Cases** - Recipes for typical operations
4. **Best Practices** - Security and error handling
5. **Troubleshooting** - Common issues and solutions

**Example Walkthrough:**
```markdown
## Send Your First Transaction

### Step 1: Start the Daemon
```bash
./bin/dinerod --regtest
```

### Step 2: Generate a Wallet Address
```bash
dinero-cli wallet.getnewaddress
# Returns: "bc1qtest..."
```

### Step 3: Mine Blocks for Spendable Coins
```bash
# Mine 101 blocks (1 coinbase + 100 for maturity)
dinero-cli generatetoaddress 101 "bc1qtest..."
```

### Step 4: Check Your Balance
```bash
dinero-cli wallet.getbalance
# Returns:
# {
#   "confirmed": 5050.0,
#   "unconfirmed": 0.0,
#   "immature": 0.0,
#   "spendable": 5050.0
# }
```

### Step 5: Send a Transaction
```bash
# Send 10 DIN to recipient
dinero-cli wallet.sendtoaddress "bc1qrecipient..." 10.0
# Returns:
# {
#   "txid": "abc123...",
#   "fee": 0.00001
# }
```

### Step 6: Mine Confirmation Block
```bash
dinero-cli generatetoaddress 1 "bc1qtest..."
```

### Step 7: Verify Transaction
```bash
dinero-cli wallet.listtransactions
# Shows your sent transaction with 1 confirmation
```

**Congratulations! You've sent your first DineroCoin transaction!**
```

---

### Week 3: Additional RPC Methods & Enhancements

#### Task 3.1: Add Missing Wallet RPC Methods

**Methods to Implement/Verify:**

1. **`wallet.createrawtransaction`** - Low-level transaction creation
```cpp
din::Json rpc_wallet_createrawtransaction(const ExecutionContext& ctx,
                                          const din::Json& params);
```

2. **`wallet.signrawtransaction`** - Sign raw transaction
```cpp
din::Json rpc_wallet_signrawtransaction(const ExecutionContext& ctx,
                                       const din::Json& params);
```

3. **`wallet.sendrawtransaction`** - Broadcast raw transaction
```cpp
din::Json rpc_wallet_sendrawtransaction(const ExecutionContext& ctx,
                                       const din::Json& params);
```

4. **`wallet.estimatefee`** - Fee estimation
```cpp
din::Json rpc_wallet_estimatefee(const ExecutionContext& ctx,
                                const din::Json& params);
```

5. **`wallet.abandontransaction`** - Abandon stuck transaction
```cpp
din::Json rpc_wallet_abandontransaction(const ExecutionContext& ctx,
                                       const din::Json& params);
```

#### Task 3.2: Add Transaction History RPC Methods

**Methods:**

1. **`wallet.gettransaction`** - Get transaction details by txid
2. **`wallet.listtransactions`** - List recent transactions (paginated)
3. **`wallet.listsinceblock`** - Transactions since block height
4. **`wallet.getwalletinfo`** - Wallet metadata and statistics

#### Task 3.3: Add UTXO Management RPC Methods

**Methods:**

1. **`wallet.listunspent`** - List unspent outputs (enhanced)
   - Filter by amount range
   - Filter by confirmations
   - Filter by address

2. **`wallet.lockunspent`** - Lock UTXOs to prevent spending
3. **`wallet.listlockunspent`** - List locked UTXOs
4. **`wallet.fundrawtransaction`** - Add inputs to raw transaction

---

## Testing Strategy

### Unit Tests
```cpp
// Test RPC parsing and parameter validation
TEST(RPC, ParameterValidation) {
    // Test valid parameters
    // Test missing parameters
    // Test invalid types
    // Test boundary values
}
```

### Integration Tests
```cpp
// Test RPC → Phase 3 components → Blockchain
TEST(RPC, EndToEndSpending) {
    // Full workflow from RPC call to confirmed transaction
}
```

### Regression Tests
```cpp
// Ensure existing RPC methods still work
TEST(RPC, BackwardCompatibility) {
    // Test all 39 wallet RPC methods
}
```

### Stress Tests
```cpp
// Test RPC under load
TEST(RPC, ConcurrentRequests) {
    // 100 concurrent RPC calls
    // Verify no race conditions
}
```

---

## Success Criteria

Phase 4 is complete when:

✅ All wallet RPC methods tested and documented
✅ RPC methods correctly use Phase 3 components
✅ Comprehensive test suite (>90% coverage)
✅ API reference documentation complete
✅ Quick start guide for developers
✅ Error handling robust
✅ Performance acceptable (< 100ms per RPC call)
✅ Security reviewed (authentication, rate limiting)

---

## Deliverables

### Code:
1. `tests/rpc/test_wallet_rpc_integration.cpp` - Integration tests
2. `tests/rpc/test_rpc_error_handling.cpp` - Error handling tests
3. `tests/rpc/test_rpc_phase3_integration.cpp` - Phase 3 integration tests
4. Any new/enhanced RPC methods in `src/rpc/`

### Documentation:
1. `docs/RPC_API_REFERENCE.md` - Complete API documentation
2. `docs/RPC_QUICK_START.md` - Getting started guide
3. `docs/RPC_SECURITY.md` - Security best practices
4. `docs/PHASE_4_COMPLETE.md` - Phase 4 completion summary

### Tests:
- Minimum 100 test cases covering all RPC methods
- Integration with Phase 3 components verified
- Error handling comprehensive
- Performance benchmarks documented

---

## Timeline

```
Week 1: RPC Testing & Verification
  ├─ Day 1-2: Create test infrastructure
  ├─ Day 3-4: Write integration tests
  └─ Day 5-7: Test Phase 3 integration

Week 2: Documentation
  ├─ Day 1-3: Write API reference
  ├─ Day 4-5: Create quick start guide
  └─ Day 6-7: Security documentation

Week 3: Enhancements & Polish
  ├─ Day 1-3: Implement missing RPC methods
  ├─ Day 4-5: Performance testing
  └─ Day 6-7: Final verification
```

---

## Phase 4 vs Existing "Phase 4 Roadmap"

**Note:** There's an existing `docs/PHASE4_ROADMAP.md` focused on production deployment and Architecture V3. That document appears to be for a different initiative (architecture migration).

**This Phase 4** is a logical continuation of:
- Phase 1: Wallet Foundation
- Phase 2: Wallet Persistence
- Phase 3: Transaction Spending & Fee Logic
- **Phase 4: RPC/API Layer** ← **Current**

The phases are:
- **Phase 1-3**: Core wallet and transaction functionality
- **Phase 4**: Expose functionality via RPC API
- **Phase 5**: Advanced features (HD wallet, multisig, etc.)

---

## Next Steps

1. ✅ Review existing RPC implementation
2. ⏳ Create test infrastructure
3. ⏳ Write integration tests
4. ⏳ Document API
5. ⏳ Add missing methods
6. ⏳ Performance testing
7. ⏳ Security review

**Status:** Ready to begin implementation!

---

**Document Status:** Planning Complete
**Last Updated:** December 22, 2025
**Next Review:** After Week 1 completion
