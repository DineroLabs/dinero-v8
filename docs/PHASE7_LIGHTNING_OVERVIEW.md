# Phase 7: Dinero Lightning Network Integration

**Status**: Design Phase
**Start Date**: 2025-01-11
**Target Completion**: 2025-02-01 (3.5 weeks)
**Prerequisites**: Phase 6C (Taproot Consensus) ✅ Complete

---

## Executive Summary

Phase 7 transforms Dinero from a modern Layer 1 blockchain into a **two-layer payment ecosystem**:

- **Layer 1**: Taproot-secured on-chain consensus (already complete)
- **Layer 2**: Lightning Network for instant, low-fee off-chain transactions

This phase leverages the Taproot, Schnorr, and Tapscript infrastructure built in Phase 6C to enable:
- ⚡ **Sub-second payments** (< 500ms latency)
- 💰 **Micro-transactions** (sub-una granularity)
- 🚀 **1,000× throughput** increase (off-chain TPS)
- 🔒 **Enhanced privacy** (no on-chain trace for off-chain flows)

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Core Components](#2-core-components)
3. [Channel Lifecycle](#3-channel-lifecycle)
4. [Payment Flow](#4-payment-flow)
5. [Taproot Integration](#5-taproot-integration)
6. [RPC Interface](#6-rpc-interface)
7. [Persistence Strategy](#7-persistence-strategy)
8. [Security Model](#8-security-model)
9. [Testing Strategy](#9-testing-strategy)
10. [Performance Targets](#10-performance-targets)
11. [Implementation Timeline](#11-implementation-timeline)

---

## 1. Architecture Overview

### 1.1 System Layers

```
┌────────────────────────────────────────────────────┐
│              Dinero Wallet Layer                   │
│  ├─ HDWallet (BIP32/BIP84/BIP86)                  │
│  ├─ TaprootTxSigner (BIP340 Schnorr)              │
│  └─ RPC: wallet.*, ln.*                           │
├────────────────────────────────────────────────────┤
│          Lightning Network Layer (NEW)             │
│  ├─ ChannelManager      (channel lifecycle)       │
│  ├─ CommitmentBuilder   (Taproot commitment txs)  │
│  ├─ HTLCManager         (payment contracts)       │
│  ├─ PaymentRouter       (multi-hop routing)       │
│  ├─ ChainMonitor        (on-chain watching)       │
│  └─ ChannelGraph        (network topology)        │
├────────────────────────────────────────────────────┤
│         DaemonContext Services (Existing)          │
│  ├─ ChainstateService   (block validation)        │
│  ├─ P2PService          (peer networking)         │
│  ├─ MempoolService      (tx pool)                 │
│  └─ ValidationQueue     (parallel validation)     │
├────────────────────────────────────────────────────┤
│           Storage Layer (RocksDB/SQLite)           │
│  ├─ cf_blocks           (chain data)              │
│  ├─ cf_utxos            (UTXO set)                │
│  ├─ cf_channels  (NEW)  (Lightning channels)      │
│  └─ cf_htlcs     (NEW)  (pending payments)        │
└────────────────────────────────────────────────────┘
```

### 1.2 File Structure

```
DineroCoin/
├── include/lightning/
│   ├── channel_manager.h        # Channel lifecycle & state
│   ├── commitment_builder.h     # Taproot commitment transactions
│   ├── htlc_manager.h           # HTLC creation & settlement
│   ├── payment_router.h         # Multi-hop route finding
│   ├── chain_monitor.h          # On-chain event watching
│   ├── channel_graph.h          # Network topology (BOLT #7)
│   ├── lightning_types.h        # Shared data structures
│   └── bolt11_invoice.h         # Lightning invoice encoding
├── src/lightning/
│   ├── channel_manager.cpp      # ~800 LOC
│   ├── commitment_builder.cpp   # ~600 LOC
│   ├── htlc_manager.cpp         # ~500 LOC
│   ├── payment_router.cpp       # ~400 LOC
│   ├── chain_monitor.cpp        # ~300 LOC
│   ├── channel_graph.cpp        # ~350 LOC
│   └── bolt11_invoice.cpp       # ~200 LOC
├── src/daemon/services/
│   └── lightning_service.cpp    # DaemonContext integration
├── src/rpc/
│   └── methods_lightning.cpp    # ln.* RPC methods
└── tests/lightning/
    ├── test_ln_basic.sh         # Channel open/close
    ├── test_ln_payment.sh       # Single-hop payment
    ├── test_ln_multihop.sh      # Multi-hop routing
    └── test_ln_breach.sh        # Breach remedy scenario
```

---

## 2. Core Components

### 2.1 ChannelManager (~800 LOC)

**Responsibilities**:
- Channel state machine (PENDING → OPEN → CLOSING → CLOSED)
- Commitment transaction versioning
- Revocation secret management
- Cooperative & unilateral close handling
- Persistence to RocksDB `cf_channels`

**Key Data Structures**:

```cpp
enum class ChannelState {
    PENDING_OPEN,        // Funding tx broadcast, awaiting confirmations
    OPEN,                // Channel active, can route payments
    PENDING_CLOSE,       // Cooperative close initiated
    FORCE_CLOSING,       // Unilateral close (breach or timeout)
    CLOSED               // On-chain settled
};

struct Channel {
    std::string channel_id;              // 32-byte unique ID
    std::string peer_node_id;            // Remote peer pubkey

    // Funding
    std::string funding_txid;
    uint32_t funding_vout;
    uint64_t funding_amount_sats;

    // Balances
    uint64_t local_balance_msats;
    uint64_t remote_balance_msats;

    // State
    ChannelState state;
    uint64_t commitment_number;          // Current commitment version
    std::vector<uint8_t> revocation_secret;

    // Taproot keys
    std::vector<uint8_t> local_funding_key;
    std::vector<uint8_t> remote_funding_key;
    std::vector<uint8_t> revocation_basepoint;

    // Pending HTLCs
    std::vector<HTLC> pending_htlcs;

    // Metadata
    uint64_t created_at;
    uint64_t last_update;
    bool is_initiator;
};
```

**Public API**:

```cpp
class ChannelManager {
public:
    explicit ChannelManager(DaemonContext& ctx);

    // Channel lifecycle
    Result<Channel> openChannel(
        const std::string& peer_node_id,
        uint64_t local_amount_sats,
        uint64_t push_amount_sats = 0
    );

    Result<void> closeChannel(
        const std::string& channel_id,
        bool force = false
    );

    // State queries
    std::optional<Channel> getChannel(const std::string& channel_id);
    std::vector<Channel> listChannels();
    uint64_t getTotalBalance() const;

    // Update handling
    Result<void> updateBalance(
        const std::string& channel_id,
        int64_t local_delta_msats
    );

    Result<void> addHTLC(
        const std::string& channel_id,
        const HTLC& htlc
    );

    Result<void> settleHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::vector<uint8_t>& preimage
    );

private:
    DaemonContext& ctx_;
    std::map<std::string, Channel> channels_;
    std::mutex channels_mutex_;

    // Persistence
    void saveChannel(const Channel& channel);
    void loadChannels();
};
```

---

### 2.2 CommitmentBuilder (~600 LOC)

**Responsibilities**:
- Build Taproot funding outputs (2-of-2 MuSig)
- Create commitment transactions with script-path spending
- Generate revocation keys and timelocks
- Sign with BIP340 Schnorr (via `TaprootTxSigner`)

**Taproot Script Tree**:

```
Commitment Output (Taproot):
├─ Key Path: MuSig2(local_key, remote_key)  [cooperative close]
└─ Script Tree:
   ├─ Leaf 1: <revocation_key> OP_CHECKSIG  [breach remedy]
   └─ Leaf 2: <delay> OP_CSV <local_key> OP_CHECKSIG  [timeout claim]
```

**Public API**:

```cpp
class CommitmentBuilder {
public:
    explicit CommitmentBuilder(DaemonContext& ctx);

    // Funding transaction (on-chain)
    Result<Transaction> buildFundingTx(
        const Channel& channel,
        const std::vector<WalletUTXO>& inputs
    );

    // Commitment transaction (off-chain, broadcast if force-close)
    Result<Transaction> buildCommitmentTx(
        const Channel& channel,
        uint64_t commitment_number,
        const std::vector<HTLC>& htlcs
    );

    // Revocation transaction (broadcast if breach detected)
    Result<Transaction> buildRevocationTx(
        const Channel& channel,
        const Transaction& old_commitment_tx,
        const std::vector<uint8_t>& revocation_secret
    );

    // HTLC timeout/success transactions
    Result<Transaction> buildHTLCTimeoutTx(const HTLC& htlc);
    Result<Transaction> buildHTLCSuccessTx(const HTLC& htlc);

private:
    DaemonContext& ctx_;

    // Taproot helpers
    TapTree buildCommitmentScriptTree(
        const std::vector<uint8_t>& local_key,
        const std::vector<uint8_t>& remote_key,
        const std::vector<uint8_t>& revocation_key,
        uint32_t timelock_blocks
    );

    std::vector<uint8_t> computeMuSigKey(
        const std::vector<uint8_t>& key1,
        const std::vector<uint8_t>& key2
    );
};
```

---

### 2.3 HTLCManager (~500 LOC)

**Responsibilities**:
- Create HTLC offers (outgoing payments)
- Handle HTLC receives (incoming payments)
- Monitor timeouts and claim windows
- Validate hash preimages

**HTLC Structure**:

```cpp
struct HTLC {
    std::string htlc_id;                 // Unique ID
    uint64_t amount_msats;
    std::vector<uint8_t> payment_hash;   // SHA256(preimage)
    uint32_t cltv_expiry;                // Absolute block height
    bool is_incoming;                    // true = receive, false = send

    // Routing
    std::string next_hop;                // Next channel_id in route
    std::string prev_hop;                // Previous channel_id

    // State
    enum State {
        PENDING,
        SETTLED,
        FAILED,
        TIMED_OUT
    } state;

    uint64_t created_at;
};
```

**Public API**:

```cpp
class HTLCManager {
public:
    explicit HTLCManager(DaemonContext& ctx);

    // HTLC lifecycle
    Result<HTLC> offerHTLC(
        const std::string& channel_id,
        uint64_t amount_msats,
        const std::vector<uint8_t>& payment_hash,
        uint32_t cltv_expiry
    );

    Result<void> settleHTLC(
        const std::string& htlc_id,
        const std::vector<uint8_t>& preimage
    );

    Result<void> failHTLC(
        const std::string& htlc_id,
        const std::string& reason
    );

    // Monitoring
    std::vector<HTLC> getExpiredHTLCs(uint32_t current_height);
    void claimTimedOutHTLCs();

private:
    DaemonContext& ctx_;
    std::map<std::string, HTLC> htlcs_;
    std::mutex htlcs_mutex_;

    // Validation
    bool validatePreimage(
        const std::vector<uint8_t>& preimage,
        const std::vector<uint8_t>& payment_hash
    );
};
```

---

### 2.4 PaymentRouter (~400 LOC)

**Responsibilities**:
- Pathfinding (Dijkstra's algorithm)
- Fee calculation per hop
- HTLC chaining across multiple channels
- Payment splitting (MPP - Multi-Part Payments)

**Public API**:

```cpp
class PaymentRouter {
public:
    explicit PaymentRouter(DaemonContext& ctx);

    // Route finding
    Result<Route> findRoute(
        const std::string& dest_node_id,
        uint64_t amount_msats,
        uint32_t max_hops = 20
    );

    // Payment execution
    Result<PaymentResult> sendPayment(
        const std::string& bolt11_invoice
    );

    Result<PaymentResult> sendSpontaneousPayment(
        const std::string& dest_node_id,
        uint64_t amount_msats
    );

private:
    DaemonContext& ctx_;
    std::shared_ptr<ChannelGraph> graph_;

    // Pathfinding
    struct Hop {
        std::string channel_id;
        std::string node_id;
        uint64_t amount_msats;
        uint64_t fee_msats;
        uint32_t cltv_delta;
    };

    struct Route {
        std::vector<Hop> hops;
        uint64_t total_amount_msats;
        uint64_t total_fee_msats;
    };
};
```

---

## 3. Channel Lifecycle

### 3.1 Channel Opening

```
Alice                                           Bob
  │                                              │
  ├─── 1. ln.openchannel bob 1000000 ──────────>│
  │                                              │
  ├─── 2. Build funding tx ────────────────────>│
  │         (2-of-2 Taproot MuSig)              │
  │                                              │
  │<──── 3. Sign funding tx ──────────────────────┤
  │                                              │
  ├─── 4. Broadcast to mempool ────────────────>│
  │         Wait 6 confirmations                 │
  │                                              │
  │<──── 5. Channel OPEN ──────────────────────────┤
  │         (both build commitment_tx v0)        │
  │                                              │
```

### 3.2 Payment Update

```
Alice wants to pay Bob 10,000 sats:

1. Alice creates HTLC:
   - payment_hash = SHA256(preimage)
   - amount = 10,000 sats
   - cltv_expiry = current_height + 144

2. Both parties build new commitment_tx v1:
   - Alice's balance: 990,000 sats
   - Bob's balance: 10,000 sats (pending HTLC)

3. Exchange revocation secrets for v0

4. Bob reveals preimage → HTLC settles

5. Build commitment_tx v2:
   - Alice's balance: 990,000 sats
   - Bob's balance: 10,000 sats (final)
```

### 3.3 Cooperative Close

```
Alice                                           Bob
  │                                              │
  ├─── 1. ln.closechannel <channel_id> ───────>│
  │                                              │
  ├─── 2. Build closing tx ────────────────────>│
  │         (key-path spend, no script)         │
  │                                              │
  │<──── 3. Sign closing tx ──────────────────────┤
  │                                              │
  ├─── 4. Broadcast to mempool ────────────────>│
  │                                              │
  │<──── 5. Channel CLOSED ────────────────────────┤
  │         (balances swept on-chain)            │
```

---

## 4. Payment Flow (Multi-Hop)

```
Alice wants to pay Carol 5,000 sats via Bob:

┌───────────────────────────────────────────────────┐
│  Step 1: Carol creates invoice                    │
│  carol> ln.createinvoice 5000 "coffee"            │
│  Returns: lndin1p...qz3m                          │
└───────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────┐
│  Step 2: Alice decodes invoice                    │
│  alice> ln.decodeinvoice lndin1p...qz3m           │
│  {                                                 │
│    "dest": "carol_node_id",                       │
│    "amount": 5000,                                │
│    "payment_hash": "abc123..."                    │
│  }                                                 │
└───────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────┐
│  Step 3: Find route                               │
│  alice> PaymentRouter::findRoute(carol, 5000)     │
│  Route: Alice → Bob → Carol                       │
│  Fees: 1 sat (Bob's routing fee)                  │
└───────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────┐
│  Step 4: Lock HTLCs (onion-routed)               │
│                                                    │
│  Alice → Bob: HTLC 5001 sats, hash=abc123         │
│  Bob → Carol: HTLC 5000 sats, hash=abc123         │
└───────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────┐
│  Step 5: Carol reveals preimage                   │
│  Carol → Bob: preimage="xyz789"                   │
│  Bob → Alice: preimage="xyz789"                   │
└───────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────┐
│  Step 6: Settle HTLCs                             │
│  Alice's balance: -5001 sats                      │
│  Bob's balance: +1 sat (routing fee)              │
│  Carol's balance: +5000 sats                      │
└───────────────────────────────────────────────────┘
```

---

## 5. Taproot Integration

### 5.1 Funding Output (On-Chain)

```
Taproot P2TR Output:
OP_1 <32-byte MuSig(alice_key, bob_key)>

Spending Paths:
1. Key Path: MuSig2 signature (cooperative close)
2. Script Path: (not used for funding, only commitments)
```

### 5.2 Commitment Output (Off-Chain Template)

```
Taproot P2TR Output:
OP_1 <32-byte tweaked_key>

Internal Key: MuSig(alice_delay_key, bob_delay_key)

Script Tree:
├─ Revocation: <revocation_pubkey> OP_CHECKSIG
└─ Timelock:   <144> OP_CSV <alice_key> OP_CHECKSIG
```

**Benefits**:
- **Small outputs**: 34 bytes vs 43 bytes (P2WSH)
- **Privacy**: Script only revealed when spending via script path
- **Efficiency**: Key-path cooperative close = single 64-byte signature

### 5.3 HTLC Output (Script Path)

```
HTLC Offered:
OP_IF
  <revocation_pubkey> OP_CHECKSIG
OP_ELSE
  <remote_pubkey> OP_SWAP
  OP_SIZE 32 OP_EQUALVERIFY
  OP_SHA256 <payment_hash> OP_EQUALVERIFY
  OP_CHECKSIG
OP_ENDIF

HTLC Received:
OP_IF
  <local_pubkey> OP_CHECKSIG
OP_ELSE
  <cltv_expiry> OP_CLTV OP_DROP
  <remote_pubkey> OP_CHECKSIG
OP_ENDIF
```

---

## 6. RPC Interface

### 6.1 Channel Management

```bash
# Open channel
./dinero-cli ln.openchannel <peer_node_id> <amount_sats> [push_sats]
# Example:
./dinero-cli ln.openchannel 03abc123... 1000000 0

# Close channel
./dinero-cli ln.closechannel <channel_id> [--force]
# Example:
./dinero-cli ln.closechannel channel_abc123... false

# List channels
./dinero-cli ln.listchannels
# Returns:
# [
#   {
#     "channel_id": "abc123...",
#     "peer": "03def456...",
#     "state": "OPEN",
#     "local_balance": 500000,
#     "remote_balance": 500000,
#     "capacity": 1000000
#   }
# ]

# Get channel info
./dinero-cli ln.getchannel <channel_id>
```

### 6.2 Payment Sending

```bash
# Send payment via invoice
./dinero-cli ln.sendpayment <bolt11_invoice>
# Example:
./dinero-cli ln.sendpayment lndin1p5u0rrkafpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqf

# Send spontaneous payment (keysend)
./dinero-cli ln.keysend <dest_node_id> <amount_msats>

# Decode invoice
./dinero-cli ln.decodeinvoice <bolt11_invoice>
```

### 6.3 Invoice Creation

```bash
# Create invoice
./dinero-cli ln.createinvoice <amount_sats> <description> [expiry_secs]
# Example:
./dinero-cli ln.createinvoice 5000 "Coffee" 3600
# Returns:
# {
#   "payment_hash": "abc123...",
#   "bolt11": "lndin1p...",
#   "expires_at": 1640000000
# }

# List invoices
./dinero-cli ln.listinvoices [--pending]
```

### 6.4 Network Info

```bash
# Get Lightning node info
./dinero-cli ln.getinfo
# Returns:
# {
#   "node_id": "03abc123...",
#   "num_channels": 5,
#   "num_active_channels": 4,
#   "total_capacity": 10000000,
#   "version": "Dinero Lightning v0.1.0"
# }

# Get network graph
./dinero-cli ln.getnodelist
./dinero-cli ln.getchannellist
```

---

## 7. Persistence Strategy

### 7.1 RocksDB Column Families

```cpp
// Channel data
cf_channels:
  Key: channel_id (32 bytes)
  Value: protobuf-encoded Channel struct

// HTLC data
cf_htlcs:
  Key: htlc_id (32 bytes)
  Value: protobuf-encoded HTLC struct

// Network graph (gossip)
cf_channel_graph:
  Key: short_channel_id (8 bytes)
  Value: ChannelAnnouncement + ChannelUpdate

cf_nodes:
  Key: node_id (33 bytes)
  Value: NodeAnnouncement
```

### 7.2 Write-Ahead Log (WAL)

```cpp
// Critical state changes must be atomic
void ChannelManager::updateChannelState(const Channel& channel) {
    rocksdb::WriteBatch batch;

    // 1. Write new channel state
    batch.Put(cf_channels, channel.channel_id, serialize(channel));

    // 2. Write commitment number increment
    batch.Put(cf_metadata, "commitment_" + channel.channel_id,
              std::to_string(channel.commitment_number));

    // 3. Atomic commit
    rocksdb::WriteOptions opts;
    opts.sync = true;  // Force fsync (critical for Lightning)
    db_->Write(opts, &batch);
}
```

---

## 8. Security Model

### 8.1 Breach Remedy

```
Scenario: Alice tries to broadcast old commitment_tx v5
          (Current state is v10)

Bob's ChainMonitor detects:
  1. Commitment tx in mempool/block
  2. Commitment number < current
  3. Has revocation secret for v5

Bob broadcasts revocation_tx:
  - Claims entire channel balance (punishment)
  - Uses revocation_basepoint + per_commitment_secret
  - Sweeps to Bob's wallet

Alice loses all funds (breach penalty)
```

### 8.2 Revocation Key Derivation

```cpp
// BIP32-style derivation
revocation_key = SHA256(revocation_basepoint || per_commitment_point)

// Each commitment version has unique revocation key
// Old secrets are revealed when updating to new state
```

### 8.3 Watchtowers (Future: Phase 7.9)

```
Watchtower Service:
- User uploads encrypted breach remedy transactions
- Watchtower monitors chain for breaches
- Automatically broadcasts penalty if user is offline
- Privacy-preserving (watchtower can't steal funds)
```

---

## 9. Testing Strategy

### 9.1 Unit Tests

```bash
# ChannelManager tests
tests/lightning/test_channel_lifecycle.cpp
  - Open channel
  - Update balance
  - Cooperative close
  - Force close

# CommitmentBuilder tests
tests/lightning/test_commitment_builder.cpp
  - Build funding tx
  - Build commitment tx with HTLCs
  - Revocation tx generation
  - Taproot script path validation

# HTLCManager tests
tests/lightning/test_htlc_manager.cpp
  - Offer HTLC
  - Settle with preimage
  - Timeout handling
  - Hash validation

# PaymentRouter tests
tests/lightning/test_payment_router.cpp
  - Route finding (Dijkstra)
  - Fee calculation
  - Multi-part payments
```

### 9.2 Integration Tests

```bash
#!/bin/bash
# tests/lightning/test_ln_network.sh

# 1. Start 3 regtest nodes
./dinero-cli -datadir=/tmp/alice -regtest &
./dinero-cli -datadir=/tmp/bob -regtest &
./dinero-cli -datadir=/tmp/carol -regtest &

# 2. Mine coins
./dinero-cli -datadir=/tmp/alice generatetoaddress 101 <alice_addr>

# 3. Open channels
alice> ln.openchannel <bob_id> 1000000
bob> ln.openchannel <carol_id> 1000000

# 4. Wait for confirmations
sleep 60

# 5. Send payment Alice → Carol (via Bob)
carol> ln.createinvoice 5000 "test"
alice> ln.sendpayment <bolt11>

# 6. Verify balances
alice> ln.listchannels  # Expect -5001 sats
bob> ln.listchannels    # Expect +1 sat (fee)
carol> ln.listchannels  # Expect +5000 sats

# 7. Close channels
alice> ln.closechannel <channel_id>
bob> ln.closechannel <channel_id>
```

### 9.3 Breach Simulation

```bash
# tests/lightning/test_ln_breach.sh

# 1. Alice opens channel with Bob
alice> ln.openchannel <bob_id> 1000000

# 2. Make 10 payments (create commitment v0-v10)
for i in {1..10}; do
  alice> ln.sendpayment <bob_invoice>
done

# 3. Alice saves old commitment_tx v5
tx_v5=$(alice> ln.debug.getcommitmenttx 5)

# 4. Continue to commitment v15
for i in {11..15}; do
  alice> ln.sendpayment <bob_invoice>
done

# 5. Alice broadcasts old tx_v5 (breach attempt)
alice> sendrawtransaction $tx_v5

# 6. Bob's ChainMonitor detects breach
# 7. Bob broadcasts revocation_tx (claims all funds)
# 8. Verify Bob received full channel balance
```

---

## 10. Performance Targets

| Metric | Target | Measurement |
|--------|--------|-------------|
| Payment Latency | < 500 ms | Time from `sendpayment` to settlement |
| Channel Open Time | < 60 sec | Funding tx confirmation (1 block) |
| Off-chain TPS | > 1,000 tx/s | Per channel pair throughput |
| Memory per Channel | < 100 KB | RocksDB storage overhead |
| Fee Overhead | < 0.01% | Routing fee as % of payment |
| Graph Sync Time | < 10 sec | Initial gossip sync (100 nodes) |
| HTLC Resolution | < 2 sec | Preimage reveal → settlement |

### 10.1 Benchmarking

```cpp
// benchmarks/ln_payment_bench.cpp
void BM_PaymentLatency(benchmark::State& state) {
    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();

        router.sendPayment(invoice);

        auto end = std::chrono::high_resolution_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
    }
}
BENCHMARK(BM_PaymentLatency)->Unit(benchmark::kMillisecond);
```

---

## 11. Implementation Timeline

| Phase | Component | Effort | Dependencies |
|-------|-----------|--------|--------------|
| 7.0 | Documentation | 1 day | None |
| 7.1 | ChannelManager + ChainMonitor | 4 days | RocksDB cf_channels |
| 7.2 | CommitmentBuilder (Taproot) | 3 days | TaprootTxSigner, TapTree |
| 7.3 | HTLCManager + FeePolicy | 2 days | TapscriptInterpreter |
| 7.4 | PaymentRouter + ChannelGraph | 4 days | BOLT #7 gossip |
| 7.5 | LightningService + RPC | 1 day | DaemonContext |
| 7.6 | Testing Suite | 2 days | Regtest nodes |
| 7.7 | Metrics + Final Docs | 1 day | Prometheus |
| **Total** | | **18 days** | **(3.5 weeks)** |

### 11.1 Milestones

- **Week 1**: Channel lifecycle (open/close) working on regtest
- **Week 2**: Single-hop payments (Alice → Bob) functional
- **Week 3**: Multi-hop routing (Alice → Bob → Carol) functional
- **Week 3.5**: Testing, documentation, metrics complete

---

## 12. Future Expansions (Phase 7+)

### 7.8 - Submarine Swaps
- On-chain ↔ off-chain atomic swaps
- Use case: Accept Lightning, receive on-chain DIN

### 7.9 - Watchtowers
- Third-party breach monitoring
- Encrypted breach remedy storage

### 7.10 - AMP (Atomic Multi-Path Payments)
- Split large payments across multiple routes
- Increases success rate + privacy

### 7.11 - Trampoline Routing
- Lightweight client support
- Delegate route-finding to relay nodes

### 7.12 - Taproot Assets (RGB Protocol)
- Issue tokens on Lightning channels
- Enables stablecoins, NFTs off-chain

---

## 13. References

- **BOLT Specifications**: https://github.com/lightning/bolts
- **BIP340**: Schnorr Signatures for secp256k1
- **BIP341**: Taproot: SegWit version 1 spending rules
- **BIP342**: Validation of Taproot Scripts
- **LDK Docs**: https://lightningdevkit.org/
- **c-lightning**: https://github.com/ElementsProject/lightning

---

## 14. Appendix: Taproot Script Examples

### A. Commitment Output Script

```
OP_IF
  # Revocation path (Bob can claim if Alice broadcasts old state)
  <revocation_pubkey>
OP_ELSE
  # Timeout path (Alice can claim after delay)
  144  # 1 day timelock
  OP_CHECKSEQUENCEVERIFY
  OP_DROP
  <alice_delayed_pubkey>
OP_ENDIF
OP_CHECKSIG
```

### B. HTLC Offered Script

```
OP_IF
  # Success path (Bob reveals preimage)
  OP_SHA256
  <payment_hash>
  OP_EQUALVERIFY
  <bob_pubkey>
OP_ELSE
  # Timeout path (Alice reclaims after CLTV)
  <cltv_expiry>
  OP_CHECKLOCKTIMEVERIFY
  OP_DROP
  <alice_pubkey>
OP_ENDIF
OP_CHECKSIG
```

---

**Document Version**: 1.0
**Last Updated**: 2025-01-11
**Author**: Dinero Core Team
**Status**: ✅ Ready for Implementation
