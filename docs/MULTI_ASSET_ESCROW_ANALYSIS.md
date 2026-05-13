# DineroCoin Multi-Asset Escrow Implementation Guide

## Executive Summary

The DineroCoin codebase has a well-designed **Bitcoin-style P2SH escrow system** that handles single-asset (DIN) escrow contracts using on-chain smart contracts with CLTV (CHECKLOCKTIMEVERIFY) timelocks. A separate **Bridge/Routing Engine** handles multi-asset conversions through DEX, hybrid, and custodial providers.

**Key Finding**: The infrastructure is already in place to support multi-asset escrow by leveraging the routing engine and extending the escrow contract system to support arbitrary assets beyond DIN.

---

## Current Architecture

### 1. Core Escrow System

#### Location
- **Header**: `/Users/haydarevich/Documents/DineroCoin/include/contracts/escrow_contract.h`
- **Implementation**: `/Users/haydarevich/Documents/DineroCoin/src/contracts/escrow_contract.cpp`
- **Manager**: `/Users/haydarevich/Documents/DineroCoin/include/p2p/escrow_manager.h`

#### Key Components

**EscrowKeys Structure**
```cpp
struct EscrowKeys {
    std::string buyer_pubkey;      // Buyer's public key (hex)
    std::string seller_pubkey;     // Seller's public key (hex)
    std::string mediator_pubkey;   // Mediator's public key (hex)
};
```

**EscrowContract Structure**
```cpp
struct EscrowContract {
    std::string contract_id;        // Unique identifier
    EscrowKeys keys;                // All public keys
    double amount;                  // DIN amount locked
    uint32_t refund_time;          // Block height for refund
    std::string redeem_script;      // Full script (hex)
    std::string script_hash;        // Hash of script
    std::string p2sh_address;       // P2SH escrow address (din1q...)
    std::string lock_txid;          // Transaction locking funds
    uint32_t lock_vout;             // Output index
    uint64_t created_at;            // Timestamp
    std::string status;             // pending/locked/released/refunded/expired
    int confirmations;              // Blockchain confirmations
};
```

#### Redeem Script Format

The script is **Bitcoin Script** with IF/ELSE branches:

```
IF (Release path):
    2 <PK_Buyer> <PK_Seller> <PK_Mediator> 3 OP_CHECKMULTISIG
    → Requires 2-of-3 signatures (normal release or dispute)

ELSE (Refund path):
    <REFUND_TIME> OP_CHECKLOCKTIMEVERIFY OP_DROP
    <PK_Buyer> OP_CHECKSIG
    → After timeout, buyer can reclaim funds alone
ENDIF
```

#### Design Strengths
- ✓ On-chain enforcement (no custodial risk)
- ✓ Timelock protection (CLTV prevents fund freezing)
- ✓ Mediator dispute resolution (2-of-3 multisig)
- ✓ No consensus changes required (standard Bitcoin Script)

#### Known Limitations
- ✗ Single-asset only (hardcoded for DIN)
- ✗ P2SH address generation incomplete (placeholder bech32 encoding)
- ✗ Transaction building stubs (no actual lock/release/refund tx creation)
- ✗ Blockchain integration incomplete (no actual UTXO spending)

### 2. EscrowContractBuilder Class

**Key Methods**:

```cpp
class EscrowContractBuilder {
    // Core contract building
    static EscrowContract buildContract(
        const EscrowKeys& keys,
        double amount,
        uint32_t refund_blocks
    );

    // Script generation
    static std::string buildRedeemScript(
        const EscrowKeys& keys,
        uint32_t refund_time
    );

    // Address generation
    static std::string hashRedeemScript(const std::string& redeem_script);
    static std::string createP2SHAddress(const std::string& script_hash);
    static std::vector<uint8_t> addressToScriptPubKey(const std::string& address);

    // Transaction creation (TODO: incomplete)
    static std::string createLockTransaction(
        const EscrowContract& contract,
        const std::string& from_address
    );
    
    static std::string createReleaseTransaction(
        const EscrowContract& contract,
        const std::string& to_address,
        const std::string& sig_buyer,
        const std::string& sig_seller
    );

    static std::string createRefundTransaction(
        const EscrowContract& contract,
        const std::string& refund_address,
        const std::string& sig_buyer
    );
};
```

### 3. EscrowManager

Manages escrow lifecycle and state transitions:

```cpp
class EscrowManager {
    std::optional<EscrowInfo> createEscrow(
        const std::string& seller_address,
        double amount,
        uint64_t duration_seconds,
        const std::string& offer_id
    );

    std::optional<std::string> releaseEscrow(
        const std::string& escrow_id,
        const std::string& buyer_address
    );

    std::optional<std::string> refundEscrow(
        const std::string& escrow_id
    );

    std::vector<EscrowInfo> listEscrows(const std::string& address = "");
    void processExpiredEscrows();  // Auto-refund cleanup
};
```

**EscrowStatus Enum**:
```cpp
enum class EscrowStatus {
    PENDING,      // Waiting for confirmations
    LOCKED,       // Funds locked, offer active
    RELEASED,     // Funds released to buyer
    REFUNDED,     // Funds returned to seller
    EXPIRED       // Escrow expired, auto-refund pending
};
```

### 4. ContractRegistry

Thread-safe singleton for contract storage:

```cpp
class ContractRegistry {
    bool storeContract(const EscrowContract& contract);
    std::optional<EscrowContract> getContract(const std::string& contract_id);
    bool updateContract(const EscrowContract& contract);
    std::vector<EscrowContract> listContracts(const std::string& address = "");
    size_t getContractCount() const;
};
```

---

## Bridge & Multi-Asset Infrastructure

### 1. FiatBridgeProvider (Abstract Base)

Located at: `/Users/haydarevich/Documents/DineroCoin/include/bridge/fiat_bridge_provider.h`

```cpp
class FiatBridgeProvider {
public:
    virtual ConversionResult convert(const ConversionRequest& req) = 0;
    virtual std::optional<double> get_rate(
        const std::string& from, 
        const std::string& to
    ) = 0;
    virtual std::string name() const = 0;
    virtual bool is_available() const { return true; }
};
```

**ConversionRequest**:
```cpp
struct ConversionRequest {
    std::string from_asset;      // e.g., "DIN"
    std::string to_asset;        // e.g., "USDT", "USDC", "USD"
    double amount;               // Amount to convert
    std::string dest_address;    // Destination wallet address
    std::string provider_hint;   // "dex", "hybrid", "custodial" (auto if empty)
    std::string webhook_url;     // Optional callback URL
    uint32_t max_slippage_bps;   // Max slippage in basis points
    uint32_t timeout_seconds;    // Max conversion time
};
```

**ConversionResult**:
```cpp
struct ConversionResult {
    bool success = false;
    std::string txid;                 // Transaction/order ID
    double received_amount = 0.0;     // Actual amount received
    double rate = 0.0;                // Exchange rate used
    std::string provider;             // Provider name
    std::string message;              // Status/error message
    double fee_amount = 0.0;          // Fee charged
    double slippage_bps = 0.0;        // Actual slippage
    uint64_t timestamp = 0;           // Unix timestamp
};
```

### 2. FiatBridgeManager

Located at: `/Users/haydarevich/Documents/DineroCoin/include/bridge/fiat_bridge_manager.h`

Orchestrates conversions across multiple providers:

```cpp
class FiatBridgeManager {
    void register_provider(const std::shared_ptr<FiatBridgeProvider>& provider);
    
    ConversionResult convert(const ConversionRequest& req);
    
    std::optional<double> get_rate(const std::string& from, const std::string& to);
    
    std::optional<double> get_rate_auto(
        const std::string& from,
        const std::string& to,
        ConversionRoute* route_info = nullptr
    );
    
    std::vector<ConversionRoute> get_all_routes(
        const std::string& from,
        const std::string& to,
        int max_hops = 3
    );

    std::vector<std::string> list_providers() const;
    std::map<std::string, double> get_cached_rates() const;
};
```

### 3. RoutingEngine

Located at: `/Users/haydarevich/Documents/DineroCoin/include/bridge/routing_engine.h`

Sophisticated multi-hop pathfinding using Dijkstra's algorithm:

```cpp
struct RouteHop {
    std::string from_asset;
    std::string to_asset;
    double rate = 0.0;
    double fee_bps = 0.0;      // Fee in basis points
    std::string provider;
};

struct ConversionRoute {
    std::vector<RouteHop> hops;
    double total_rate = 0.0;    // Combined rate across hops
    double total_fee_bps = 0.0; // Total fees
    double slippage_bps = 0.0;  // Expected slippage
    int hop_count = 0;
    
    std::string description() const;      // e.g., "DIN→BTC→USD via dex+coinbase"
    double effective_rate() const;        // Rate after fees/slippage
};

class RoutingEngine {
    static std::optional<ConversionRoute> find_best_route(
        const std::string& from,
        const std::string& to,
        const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
        int max_hops = 3
    );

    static std::vector<ConversionRoute> find_all_routes(
        const std::string& from,
        const std::string& to,
        const std::vector<std::shared_ptr<FiatBridgeProvider>>& providers,
        int max_hops = 3
    );
};
```

**Supported Intermediate Assets**: BTC, ETH, USDT, USDC, DAI, BUSD

---

## RPC Interface

### 1. Contract Methods

Located at: `/Users/haydarevich/Documents/DineroCoin/src/rpc/methods_contract.cpp`

**contract.createescrow**
```bash
createescrow <buyer_pubkey> <seller_pubkey> <mediator_pubkey> <amount> [refund_blocks]
```

Example:
```json
{
  "contract_id": "contract_1a2b3c...",
  "p2sh_address": "din1qxyz...",
  "redeem_script": "6352210abc...",
  "script_hash": "abc123...",
  "amount": 100.0,
  "refund_time": 145680,
  "status": "pending",
  "keys": {
    "buyer": "02abc123...",
    "seller": "03def456...",
    "mediator": "04ghi789..."
  }
}
```

**contract.status**
```bash
status <contract_id>
```

**contract.release**
```bash
release <contract_id> <to_address> <sig_buyer> <sig_seller>
```

**contract.refund**
```bash
refund <contract_id> <refund_address> <sig_buyer>
```

**contract.list**
```bash
list [address]
```

### 2. Bridge Methods

Located at: `/Users/haydarevich/Documents/DineroCoin/src/rpc/methods_bridge.cpp`

**bridge.getrate**
```bash
getrate <from_asset> <to_asset>
```

**bridge.convert**
```bash
convert <from_asset> <to_asset> <amount> [provider_hint]
```

**bridge.providers**
```bash
providers
```

**bridge.status**
```bash
status
```

---

## Implementation Status

### Completed Components
- ✓ Bitcoin Script generation (IF/ELSE branches, multisig, timelock)
- ✓ Redeem script builder
- ✓ Script hashing (HASH160)
- ✓ Contract data structures
- ✓ Escrow manager state machine
- ✓ Contract registry (in-memory storage)
- ✓ RPC command stubs
- ✓ Bridge provider abstraction
- ✓ Multi-hop routing engine (Dijkstra pathfinding)
- ✓ Rate caching infrastructure

### TODO / Incomplete
- ⚠️ P2SH address generation (placeholder implementation)
- ⚠️ Actual transaction building (lock/release/refund are stubs)
- ⚠️ Blockchain integration (no actual UTXO spending)
- ⚠️ PSBT support for contract spends
- ⚠️ Multi-asset escrow extension

---

## Suggested Approach for Multi-Asset Escrow

### Phase 1: Asset Abstraction Layer

**Create new data structure**:
```cpp
struct AssetEscrowContract : public EscrowContract {
    std::string asset_id;           // "DIN", "BTC", "USDT", etc.
    uint8_t decimals;               // Asset decimal places
    std::string locktime_path;      // Either block height or Unix timestamp
    std::optional<ConversionRoute> swap_route;  // For non-native assets
};
```

**Extend builder**:
```cpp
class MultiAssetEscrowBuilder {
    static AssetEscrowContract buildMultiAssetContract(
        const EscrowKeys& keys,
        const std::string& asset_id,
        double amount,
        uint32_t refund_blocks
    );

    static std::optional<AssetEscrowContract> buildWithAutoSwap(
        const EscrowKeys& keys,
        const std::string& source_asset,
        const std::string& escrow_asset,
        double amount,
        uint32_t refund_blocks,
        const RoutingEngine& router
    );
};
```

### Phase 2: Extended Registry

**Support multiple assets**:
```cpp
class MultiAssetContractRegistry : public ContractRegistry {
    // Index by asset type
    std::map<std::string, std::vector<EscrowContract>> contracts_by_asset;
    
    std::vector<AssetEscrowContract> listByAsset(const std::string& asset_id);
    double getTotalLockedByAsset(const std::string& asset_id);
};
```

### Phase 3: Bridge Integration

**Automatic asset conversion on release**:
```cpp
class BridgedEscrowManager : public EscrowManager {
    std::optional<std::string> releaseWithConversion(
        const std::string& escrow_id,
        const std::string& buyer_address,
        const std::string& target_asset    // Destination asset
    );

    // Refund with conversion to original asset
    std::optional<std::string> refundWithConversion(
        const std::string& escrow_id,
        const std::string& seller_address,
        const std::string& target_asset
    );
};
```

**Flow**:
1. Escrow created in USDT
2. Seller funded by swapping DIN→USDT via bridge
3. Release: Buyer's funds go through reverse swap (USDT→seller's preferred asset)
4. Automatic routing to find best conversion path

### Phase 4: RPC Methods

**New commands**:
```bash
multiasset.createescrow <buyer_pk> <seller_pk> <mediator_pk> <asset> <amount> [refund_blocks]

multiasset.releasetoasset <escrow_id> <buyer_addr> <target_asset> <sig_buyer> <sig_seller>

multiasset.listebyasset <asset_id>

multiasset.estimateswap <from_asset> <to_asset> <amount>  # Preview conversion routes
```

### Phase 5: Leverage Existing Code

**Reuse**:
- `RoutingEngine::find_best_route()` for automatic path selection
- `FiatBridgeManager` for executing conversions
- `ConversionRoute` for tracking multi-hop swaps
- Existing witness/PSBT infrastructure for complex spends

---

## Code Patterns to Follow

### 1. Bitcoin Script Building

**Pattern** (from `escrow_contract.cpp`):
```cpp
// Helper: Convert hex string to bytes
static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

// Helper: Convert bytes to hex string
static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return oss.str();
}
```

### 2. Registry Pattern

**Thread-safe singleton** (from `contract_registry.h`):
```cpp
class ContractRegistry {
private:
    ContractRegistry() = default;
    ~ContractRegistry() = default;
    ContractRegistry(const ContractRegistry&) = delete;
    ContractRegistry& operator=(const ContractRegistry&) = delete;

    std::map<std::string, EscrowContract> contracts_;
    mutable std::mutex mutex_;

public:
    static ContractRegistry& instance() {
        static ContractRegistry instance;
        return instance;
    }
};
```

### 3. Error Handling

**Pattern** (from RPC methods):
```cpp
try {
    // Parse and validate
    din::Json p = parseParams(params);
    
    if (!p.isMember("required_field")) {
        din::Json error;
        error["error"] = "Missing 'required_field' parameter";
        return error;
    }
    
    // Execute
    EscrowContract contract = EscrowContractBuilder::buildContract(...);
    
    // Verify
    if (!EscrowContractBuilder::verifyContract(contract)) {
        din::Json error;
        error["error"] = "Contract verification failed";
        return error;
    }
    
    // Log success
    dinero::g_logger.info("[MyComponent] Operation successful");
    
    // Return result
    din::Json result = contractToJson(contract);
    result["success"] = true;
    return result;
    
} catch (const std::exception& e) {
    din::Json error;
    error["error"] = std::string("operation error: ") + e.what();
    return error;
}
```

### 4. JSON Serialization

**Pattern**:
```cpp
static din::Json contractToJson(const EscrowContract& contract) {
    din::Json result;
    
    result["contract_id"] = contract.contract_id;
    result["amount"] = contract.amount;
    result["status"] = contract.status;
    result["confirmations"] = contract.confirmations;
    result["created_at"] = static_cast<Json::UInt64>(contract.created_at);

    din::Json keys;
    keys["buyer"] = contract.keys.buyer_pubkey;
    keys["seller"] = contract.keys.seller_pubkey;
    keys["mediator"] = contract.keys.mediator_pubkey;
    result["keys"] = keys;

    return result;
}
```

---

## File Organization

```
DineroCoin/
├── include/
│   ├── contracts/
│   │   ├── escrow_contract.h           (Core contract structs/builder)
│   │   └── contract_registry.h         (Contract storage)
│   ├── p2p/
│   │   └── escrow_manager.h            (Lifecycle management)
│   ├── bridge/
│   │   ├── fiat_bridge_provider.h      (Provider abstraction)
│   │   ├── fiat_bridge_manager.h       (Central orchestrator)
│   │   └── routing_engine.h            (Multi-hop pathfinding)
│   └── rpc/
│       └── methods_contract.h          (RPC interface)
│
├── src/
│   ├── contracts/
│   │   └── escrow_contract.cpp         (Script building logic)
│   ├── p2p/
│   │   └── escrow_manager.cpp          (Escrow lifecycle)
│   ├── bridge/
│   │   ├── fiat_bridge_manager.cpp     (Provider orchestration)
│   │   └── routing_engine.cpp          (Dijkstra pathfinding)
│   └── rpc/
│       └── methods_contract.cpp        (RPC implementation)
│
└── docs/
    └── SMART_CONTRACT_ESCROW.md        (Technical blueprint)
```

---

## Key Technical Insights

### 1. Script Structure Flexibility

The IF/ELSE redeem script pattern is very flexible:
- Release path (IF branch): Can be extended with additional conditions
- Refund path (ELSE branch): Can include recovery mechanisms
- Can add OP_CLTV (block height) or OP_CSV (relative locktime) variants

For multi-asset: The script itself doesn't care about asset type - only the escrow manager needs to track which asset is locked.

### 2. Address Generation

Currently uses placeholder bech32, but infrastructure is in place:
```cpp
// From escrow_contract.cpp line 237-244
auto result = bech32::Decode(hrp, address);
std::vector<uint8_t> scriptPubKey;
scriptPubKey.push_back(static_cast<uint8_t>(result->witver));
scriptPubKey.push_back(static_cast<uint8_t>(result->program.size()));
scriptPubKey.insert(scriptPubKey.end(), result->program.begin(), result->program.end());
```

### 3. Transaction Building

Current implementation is stub (returns empty strings), but transaction infrastructure exists:
```cpp
dinero::Transaction tx;
tx.version = 2;
tx.lockTime = 0;
tx.is_segwit = true;

dinero::TxInput input;
input.prevout.txid = contract.lock_txid;
input.witness.push_back({0x00});  // OP_0 for CHECKMULTISIG bug
input.witness.push_back(hexToBytes(sig_buyer));
// ... more witness stack

tx.vin.push_back(input);
std::string tx_hex = tx.SerializeHex();
```

### 4. Routing Engine Capabilities

Already supports:
- Direct conversions: DIN→USDT (1 hop)
- Multi-hop: DIN→BTC→USD (2 hops)
- Reverse routes: USD→BTC→DIN
- All-routes enumeration with sorting by effective rate
- Fee and slippage accounting

Perfect foundation for multi-asset escrow releases:
1. Seller wants USDC
2. Escrow holds USDT
3. Routing finds: USDT→BTC→USDC (or direct if available)
4. Automatic conversion on release

---

## Next Steps for Implementation

1. **Extend EscrowContract** with asset_id field
2. **Create MultiAssetEscrowBuilder** extending EscrowContractBuilder
3. **Update ContractRegistry** to index by asset
4. **Integrate RoutingEngine** for conversion selection
5. **Add RPC methods**: multiasset.createescrow, multiasset.releasetoasset
6. **Add tests** for:
   - Single-hop conversions (DIN→USDT)
   - Multi-hop conversions (DIN→BTC→USDC)
   - Automatic route selection
   - Fee/slippage accounting
7. **Documentation**: Update SMART_CONTRACT_ESCROW.md with multi-asset examples

---

## Conclusion

DineroCoin has a **solid foundation** for multi-asset escrow:
- ✓ Sophisticated routing engine (already handles multi-hop)
- ✓ Provider abstraction (supports DEX/hybrid/custodial)
- ✓ Escrow contract system (IF/ELSE script flexibility)
- ✓ Thread-safe storage layer
- ✓ Clean separation of concerns

**Main work required**: Connecting these pieces together with asset-aware escrow contracts and automatic conversion logic. The building blocks are already there—primarily needs orchestration and testing.

