# Phase 32: Cross-Network Interoperability Layer

## Design Document v1.0

**Status:** PLANNED (Not Implemented)
**Prerequisites:** Phase 30 (Taproot Assets), Phase 31 (Multi-Asset Lightning)
**Complexity:** High
**Estimated Effort:** Significant

---

## 1. Executive Summary

Phase 32 introduces cross-chain interoperability capabilities to Dinero, enabling:
- Cross-chain atomic swaps (BTC <-> DIN)
- Asset teleportation between blockchains
- SPV-verified cross-chain proofs
- Multi-network DEX routing

### Design Philosophy
- **Trustless where possible** - Use cryptographic proofs over trusted parties
- **Covenant-secured** - Leverage CTV/CCV for state transitions
- **Lightning-native** - Build on existing HTLC infrastructure
- **Incremental deployment** - Start with BTC, expand to other chains

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    DINERO INTEROP LAYER                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  Bitcoin     │  │  Ethereum    │  │   Solana     │          │
│  │  Light       │  │  Light       │  │   Light      │          │
│  │  Client      │  │  Client      │  │   Client     │          │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘          │
│         │                 │                 │                   │
│         └────────────┬────┴────────────────┘                   │
│                      │                                          │
│              ┌───────▼───────┐                                  │
│              │  Cross-Chain  │                                  │
│              │  Proof        │                                  │
│              │  Verifier     │                                  │
│              └───────┬───────┘                                  │
│                      │                                          │
│  ┌───────────────────▼───────────────────┐                     │
│  │         INTEROP LEDGER                 │                     │
│  │  - Lock tracking                       │                     │
│  │  - Claim verification                  │                     │
│  │  - Replay protection                   │                     │
│  └───────────────────┬───────────────────┘                     │
│                      │                                          │
│  ┌───────────────────▼───────────────────┐                     │
│  │      CROSS-CHAIN HTLC ENGINE          │                     │
│  │  - Atomic swaps                        │                     │
│  │  - Asset teleportation                 │                     │
│  │  - Multi-hop routing                   │                     │
│  └───────────────────────────────────────┘                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Implementation Phases

### Phase 32a: DIN <-> BTC Atomic Swaps (Foundation)
- Cross-chain HTLC structures
- Bitcoin SPV proof verification
- InteropLedger for tracking
- Basic swap coordination

### Phase 32b: Bitcoin Light Client
- Block header chain verification
- Merkle proof validation
- Difficulty adjustment verification
- Reorg handling

### Phase 32c: Asset Locks with SPV Proofs
- Lock verification via SPV
- Covenant-controlled claims
- Timeout/refund paths
- Full atomic swap flow

### Phase 32d: Ethereum Interop (Future)
- Patricia Merkle trie verification
- ETH light client
- ERC-20 support

### Phase 32e: Universal Settlement (Future)
- Multi-chain routing
- DEX integration
- Liquidity aggregation

---

## 4. Core Data Structures

### 4.1 Chain Identifiers

```cpp
// include/interop/chain_id.h

namespace dinero {
namespace interop {

enum class ChainId : uint32_t {
    DINERO_MAINNET = 0x00000001,
    DINERO_TESTNET = 0x00000002,
    DINERO_REGTEST = 0x00000003,

    BITCOIN_MAINNET = 0x00010001,
    BITCOIN_TESTNET = 0x00010002,
    BITCOIN_REGTEST = 0x00010003,

    ETHEREUM_MAINNET = 0x00020001,
    ETHEREUM_GOERLI  = 0x00020002,

    SOLANA_MAINNET = 0x00030001,
    SOLANA_DEVNET  = 0x00030002,
};

struct ChainConfig {
    ChainId chain_id;
    std::string name;
    uint32_t block_time_seconds;
    uint32_t confirmation_depth;
    bool supports_htlc;
    bool supports_spv;
};

} // namespace interop
} // namespace dinero
```

### 4.2 Cross-Chain Lock

```cpp
// include/interop/cross_chain_htlc.h

namespace dinero {
namespace interop {

/**
 * Represents funds locked on a source chain for cross-chain swap
 */
struct CrossChainLock {
    // Identifiers
    std::array<uint8_t, 32> lock_id;          // Unique lock identifier
    std::array<uint8_t, 32> lock_txid;        // Transaction ID on source chain
    uint32_t lock_vout;                        // Output index

    // Chain information
    ChainId source_chain;
    ChainId destination_chain;

    // Asset details
    std::array<uint8_t, 32> asset_id;         // Asset being locked (native = zeros)
    uint64_t amount;

    // HTLC parameters
    std::array<uint8_t, 32> payment_hash;     // SHA256(preimage)
    uint32_t timeout_height;                   // Absolute block height for timeout
    uint32_t timeout_timestamp;                // Unix timestamp for timeout

    // Participants
    std::vector<uint8_t> sender_pubkey;       // 33-byte compressed pubkey
    std::vector<uint8_t> receiver_pubkey;

    // Proof of lock (populated after confirmation)
    std::optional<SPVProof> spv_proof;

    // State
    enum class State {
        PENDING,      // Lock tx broadcast but unconfirmed
        CONFIRMED,    // Lock confirmed with sufficient depth
        CLAIMED,      // Receiver claimed with preimage
        REFUNDED,     // Sender refunded after timeout
        EXPIRED       // Timeout passed, awaiting refund
    };
    State state = State::PENDING;

    // Validation
    bool validate() const;
    std::array<uint8_t, 32> computeHash() const;

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::optional<CrossChainLock> deserialize(const std::vector<uint8_t>& data);
};

/**
 * Claim event for cross-chain lock
 */
struct CrossChainClaim {
    std::array<uint8_t, 32> lock_id;          // Reference to lock
    std::array<uint8_t, 32> preimage;         // HTLC preimage
    std::array<uint8_t, 32> claim_txid;       // Claim transaction ID
    ChainId claim_chain;                       // Chain where claim occurred

    // Proof of claim
    std::optional<SPVProof> spv_proof;

    bool validate(const CrossChainLock& lock) const;

    std::vector<uint8_t> serialize() const;
    static std::optional<CrossChainClaim> deserialize(const std::vector<uint8_t>& data);
};

/**
 * Refund event for expired cross-chain lock
 */
struct CrossChainRefund {
    std::array<uint8_t, 32> lock_id;
    std::array<uint8_t, 32> refund_txid;
    ChainId refund_chain;

    std::optional<SPVProof> spv_proof;

    bool validate(const CrossChainLock& lock, uint32_t current_height) const;
};

} // namespace interop
} // namespace dinero
```

### 4.3 SPV Proof Structures

```cpp
// include/interop/spv_proof.h

namespace dinero {
namespace interop {

/**
 * Bitcoin block header (80 bytes)
 */
struct BitcoinBlockHeader {
    int32_t version;
    std::array<uint8_t, 32> prev_block_hash;
    std::array<uint8_t, 32> merkle_root;
    uint32_t timestamp;
    uint32_t bits;                            // Difficulty target
    uint32_t nonce;

    std::array<uint8_t, 32> getHash() const;  // Double SHA256
    uint256_t getTarget() const;              // Expanded difficulty target
    bool validatePoW() const;                 // Check hash < target

    std::vector<uint8_t> serialize() const;
    static std::optional<BitcoinBlockHeader> deserialize(const std::vector<uint8_t>& data);
};

/**
 * Merkle proof for transaction inclusion
 */
struct MerkleInclusionProof {
    std::array<uint8_t, 32> tx_hash;
    std::vector<std::array<uint8_t, 32>> siblings;
    std::vector<bool> directions;             // true = left, false = right
    uint32_t tx_index;

    std::array<uint8_t, 32> computeRoot() const;
    bool verify(const std::array<uint8_t, 32>& expected_root) const;
};

/**
 * Complete SPV proof
 */
struct SPVProof {
    // Block headers proving chain of work
    std::vector<BitcoinBlockHeader> headers;

    // Merkle proof for tx inclusion
    MerkleInclusionProof merkle_proof;

    // The transaction itself
    std::vector<uint8_t> raw_transaction;

    // Chain context
    ChainId chain;
    uint32_t block_height;
    uint32_t confirmations;

    // Verification
    bool verify(const ChainConfig& config) const;
    bool verifyChainOfWork(uint32_t min_confirmations) const;
    bool verifyTxInclusion() const;

    std::vector<uint8_t> serialize() const;
    static std::optional<SPVProof> deserialize(const std::vector<uint8_t>& data);
};

} // namespace interop
} // namespace dinero
```

### 4.4 Interop Ledger

```cpp
// include/interop/interop_ledger.h

namespace dinero {
namespace interop {

/**
 * Ledger entry tracking cross-chain operations
 */
struct InteropLedgerEntry {
    std::array<uint8_t, 32> entry_id;

    enum class EntryType {
        LOCK_OUTBOUND,    // We locked funds for outgoing swap
        LOCK_INBOUND,     // External chain locked funds for incoming
        CLAIM,            // Claim executed
        REFUND,           // Refund executed
        TELEPORT_OUT,     // Asset burned for teleportation
        TELEPORT_IN       // Asset minted from teleportation
    };
    EntryType type;

    // Reference data
    std::array<uint8_t, 32> lock_id;
    std::array<uint8_t, 32> asset_id;
    uint64_t amount;
    ChainId source_chain;
    ChainId dest_chain;

    // Timestamps
    uint64_t created_at;
    uint64_t updated_at;

    // State
    bool finalized = false;

    std::vector<uint8_t> serialize() const;
    static std::optional<InteropLedgerEntry> deserialize(const std::vector<uint8_t>& data);
};

/**
 * Interop ledger database interface
 */
class InteropLedger {
public:
    // Lock operations
    bool recordLock(const CrossChainLock& lock);
    bool recordClaim(const CrossChainClaim& claim);
    bool recordRefund(const CrossChainRefund& refund);

    // Queries
    std::optional<CrossChainLock> getLock(const std::array<uint8_t, 32>& lock_id) const;
    std::vector<CrossChainLock> getPendingLocks(ChainId chain) const;
    std::vector<CrossChainLock> getExpiredLocks(uint32_t current_height) const;

    // Replay protection
    bool isLockProcessed(const std::array<uint8_t, 32>& lock_id) const;
    bool isClaimProcessed(const std::array<uint8_t, 32>& lock_id) const;

    // Statistics
    uint64_t getTotalLockedAmount(ChainId chain, const std::array<uint8_t, 32>& asset_id) const;
    uint64_t getTotalSwapVolume(ChainId src, ChainId dst) const;

private:
    std::unordered_map<std::array<uint8_t, 32>, InteropLedgerEntry> entries_;
    mutable std::mutex mutex_;
};

} // namespace interop
} // namespace dinero
```

### 4.5 Cross-Chain HTLC Engine

```cpp
// include/interop/cross_chain_engine.h

namespace dinero {
namespace interop {

/**
 * Cross-chain swap request
 */
struct CrossChainSwapRequest {
    // Swap details
    std::string swap_id;
    ChainId source_chain;
    ChainId dest_chain;

    // What we're giving
    std::array<uint8_t, 32> give_asset;
    uint64_t give_amount;

    // What we want
    std::array<uint8_t, 32> want_asset;
    uint64_t want_amount_min;

    // HTLC parameters
    std::array<uint8_t, 32> payment_hash;     // We generate preimage
    uint32_t htlc_timeout_blocks;

    // Participants
    std::vector<uint8_t> our_pubkey;
    std::vector<uint8_t> counterparty_pubkey;

    bool validate() const;
};

/**
 * Cross-chain swap state machine
 */
class CrossChainSwap {
public:
    enum class State {
        INITIATED,            // Swap request created
        AWAITING_LOCK,        // Waiting for counterparty lock
        LOCKED,               // Both sides locked
        CLAIMING,             // Claim in progress
        COMPLETED,            // Swap successful
        REFUNDING,            // Refund in progress
        REFUNDED,             // Swap cancelled/refunded
        FAILED                // Swap failed
    };

    CrossChainSwap(const CrossChainSwapRequest& request);

    // State transitions
    bool initiatorLock(const CrossChainLock& lock);
    bool responderLock(const CrossChainLock& lock);
    bool submitClaim(const CrossChainClaim& claim);
    bool submitRefund(const CrossChainRefund& refund);

    // Getters
    State getState() const { return state_; }
    const CrossChainSwapRequest& getRequest() const { return request_; }

private:
    CrossChainSwapRequest request_;
    State state_ = State::INITIATED;

    std::optional<CrossChainLock> initiator_lock_;
    std::optional<CrossChainLock> responder_lock_;
    std::optional<CrossChainClaim> claim_;
    std::optional<CrossChainRefund> refund_;
};

/**
 * Cross-chain swap engine
 */
class CrossChainEngine {
public:
    CrossChainEngine(InteropLedger& ledger);

    // Swap lifecycle
    Result<std::string> initiateSwap(const CrossChainSwapRequest& request);
    Result<void> processIncomingLock(const CrossChainLock& lock, const SPVProof& proof);
    Result<void> processClaim(const CrossChainClaim& claim, const SPVProof& proof);
    Result<void> processRefund(const CrossChainRefund& refund, const SPVProof& proof);

    // Script generation
    std::vector<uint8_t> generateHTLCScript(
        const std::array<uint8_t, 32>& payment_hash,
        const std::vector<uint8_t>& receiver_pubkey,
        const std::vector<uint8_t>& sender_pubkey,
        uint32_t timeout_height
    );

    // Bitcoin-specific
    std::vector<uint8_t> generateBitcoinHTLC(
        const std::array<uint8_t, 32>& payment_hash,
        const std::vector<uint8_t>& receiver_pubkey,
        const std::vector<uint8_t>& sender_pubkey,
        uint32_t timeout_blocks
    );

private:
    InteropLedger& ledger_;
    std::unordered_map<std::string, CrossChainSwap> active_swaps_;
};

} // namespace interop
} // namespace dinero
```

---

## 5. Bitcoin Light Client

### 5.1 Header Chain Store

```cpp
// include/interop/btc_light_client.h

namespace dinero {
namespace interop {

/**
 * Bitcoin light client for SPV verification
 */
class BitcoinLightClient {
public:
    BitcoinLightClient(ChainId chain);

    // Header management
    bool addHeader(const BitcoinBlockHeader& header);
    bool addHeaders(const std::vector<BitcoinBlockHeader>& headers);

    // Chain queries
    std::optional<BitcoinBlockHeader> getHeader(uint32_t height) const;
    std::optional<BitcoinBlockHeader> getHeaderByHash(
        const std::array<uint8_t, 32>& hash) const;
    uint32_t getBestHeight() const;
    std::array<uint8_t, 32> getBestHash() const;

    // SPV verification
    bool verifySPVProof(const SPVProof& proof) const;
    bool verifyTransaction(
        const std::vector<uint8_t>& raw_tx,
        const MerkleInclusionProof& proof,
        uint32_t block_height
    ) const;

    // Difficulty verification
    bool verifyDifficultyTransition(
        const BitcoinBlockHeader& prev,
        const BitcoinBlockHeader& curr
    ) const;

    // Reorg handling
    uint32_t getReorgDepth(const std::array<uint8_t, 32>& block_hash) const;

private:
    ChainId chain_;
    std::vector<BitcoinBlockHeader> headers_;
    std::unordered_map<std::array<uint8_t, 32>, uint32_t> hash_to_height_;

    // Checkpoints for faster sync
    std::vector<std::pair<uint32_t, std::array<uint8_t, 32>>> checkpoints_;

    // Difficulty adjustment parameters
    static constexpr uint32_t DIFFICULTY_ADJUSTMENT_INTERVAL = 2016;
    static constexpr uint32_t TARGET_TIMESPAN = 14 * 24 * 60 * 60; // 2 weeks
};

} // namespace interop
} // namespace dinero
```

---

## 6. Consensus Rules

### 6.1 Interop Validation

```cpp
// include/consensus/interop_rules.h

namespace dinero {
namespace consensus {

/**
 * Validate cross-chain lock claim
 */
bool CheckCrossChainClaim(
    const interop::CrossChainClaim& claim,
    const interop::CrossChainLock& lock,
    const interop::SPVProof& proof,
    const interop::InteropLedger& ledger
);

/**
 * Validate cross-chain refund
 */
bool CheckCrossChainRefund(
    const interop::CrossChainRefund& refund,
    const interop::CrossChainLock& lock,
    uint32_t current_height,
    const interop::InteropLedger& ledger
);

/**
 * Validate asset teleportation (burn -> mint)
 */
bool CheckAssetTeleport(
    const interop::CrossChainLock& burn_proof,  // Proof of burn on source
    const assets::AssetID& asset_id,
    uint64_t amount,
    const interop::InteropLedger& ledger
);

} // namespace consensus
} // namespace dinero
```

---

## 7. RPC Interface

```cpp
// include/rpc/methods_interop.h

// === Cross-Chain Swap RPCs ===

// interop.initiateswap
// Initiate a cross-chain atomic swap
{
    "method": "interop.initiateswap",
    "params": {
        "source_chain": "dinero",
        "dest_chain": "bitcoin",
        "give_asset": "native",
        "give_amount": 100.0,
        "want_asset": "btc",
        "want_amount_min": 0.001,
        "counterparty_pubkey": "02abc..."
    }
}

// interop.submitlockproof
// Submit SPV proof of counterparty lock
{
    "method": "interop.submitlockproof",
    "params": {
        "swap_id": "swap-123",
        "spv_proof": "..."
    }
}

// interop.claimswap
// Claim a cross-chain swap (reveal preimage)
{
    "method": "interop.claimswap",
    "params": {
        "swap_id": "swap-123",
        "preimage": "abc123..."
    }
}

// interop.refundswap
// Refund an expired cross-chain swap
{
    "method": "interop.refundswap",
    "params": {
        "swap_id": "swap-123"
    }
}

// === Light Client RPCs ===

// interop.submitbtcheaders
// Submit Bitcoin block headers for light client
{
    "method": "interop.submitbtcheaders",
    "params": {
        "headers": ["..."]
    }
}

// interop.getbtcheight
// Get current Bitcoin light client height
{
    "method": "interop.getbtcheight"
}

// === Query RPCs ===

// interop.getswapstatus
{
    "method": "interop.getswapstatus",
    "params": {
        "swap_id": "swap-123"
    }
}

// interop.listactiveswaps
{
    "method": "interop.listactiveswaps"
}
```

---

## 8. File Structure

```
include/interop/
    chain_id.h              # Chain identifiers and config
    cross_chain_htlc.h      # CrossChainLock, Claim, Refund
    spv_proof.h             # SPV proof structures
    interop_ledger.h        # Ledger for tracking operations
    cross_chain_engine.h    # Swap engine
    btc_light_client.h      # Bitcoin light client

src/interop/
    chain_id.cpp
    cross_chain_htlc.cpp
    spv_proof.cpp
    interop_ledger.cpp
    cross_chain_engine.cpp
    btc_light_client.cpp

include/consensus/
    interop_rules.h         # Consensus validation rules

src/consensus/
    interop_rules.cpp

include/rpc/
    methods_interop.h       # RPC method declarations

src/rpc/
    methods_interop.cpp     # RPC implementations

tests/
    test_cross_chain_htlc.cpp
    test_spv_proof.cpp
    test_interop_ledger.cpp
    test_btc_light_client.cpp
    test_cross_chain_swap.cpp
```

---

## 9. Security Considerations

### 9.1 Trust Model

| Component | Trust Level | Notes |
|-----------|-------------|-------|
| SPV Proofs | Cryptographic | Requires sufficient confirmations |
| HTLC Atomicity | Cryptographic | Hash preimage reveal |
| Timeout Handling | Protocol | Must handle clock skew |
| Light Client | Partial | Can be fooled by 51% attack |

### 9.2 Attack Vectors

1. **SPV Eclipse Attack**
   - Mitigation: Multiple header sources, checkpoints

2. **Preimage Withholding**
   - Mitigation: Proper timeout margins between chains

3. **Replay Attacks**
   - Mitigation: InteropLedger tracking, domain separation

4. **Clock Manipulation**
   - Mitigation: Use block heights over timestamps

### 9.3 Recommended Parameters

```cpp
// Minimum confirmations for SPV proofs
constexpr uint32_t BTC_MIN_CONFIRMATIONS = 6;
constexpr uint32_t DIN_MIN_CONFIRMATIONS = 10;

// HTLC timeout margins (blocks)
constexpr uint32_t INITIATOR_TIMEOUT = 288;  // ~48 hours BTC
constexpr uint32_t RESPONDER_TIMEOUT = 144;  // ~24 hours BTC

// Maximum swap amount without additional verification
constexpr uint64_t MAX_SWAP_AMOUNT_UNA = 100000000; // 1 BTC
```

---

## 10. Future Extensions

### 10.1 Ethereum Support
- Patricia Merkle trie verification
- EVM event log proofs
- ERC-20 token support

### 10.2 Solana Support
- Account state proofs
- Program execution verification

### 10.3 Advanced Features
- Multi-hop cross-chain routing
- Submarine swaps (LN <-> on-chain)
- Cross-chain DEX aggregation
- Trustless asset teleportation via fraud proofs

---

## 11. References

- [BIP-199: Hashed Time-Locked Contract](https://github.com/bitcoin/bips/blob/master/bip-0199.mediawiki)
- [Atomic Swaps](https://en.bitcoin.it/wiki/Atomic_swap)
- [SPV Security Model](https://bitcoin.org/en/operating-modes-guide#simplified-payment-verification-spv)
- [Bitcoin Difficulty Adjustment](https://en.bitcoin.it/wiki/Difficulty)

---

## 12. Changelog

- **v1.0** (2024-12-11): Initial design document
