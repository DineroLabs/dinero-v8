# Phase 3: lightningd Binary Architecture

**Status:** Planned (Post-Phase 3 Execution)
**Purpose:** Define the final `lightningd` standalone binary structure
**Goal:** Complete L1/L2 separation - lightningd as pure gRPC client

---

## Executive Summary

After Phase 3 completes, `lightningd` will be a **standalone L2 daemon** that:
- Runs as a separate process from `dinerod`
- Communicates with `dinerod` **only** via gRPC
- Links **zero** consensus or wallet libraries
- Can be compiled independently
- Enables external Lightning implementations

---

## Binary Layout Overview

```
┌─────────────────────────────────────────────────────┐
│ lightningd (L2 Process)                             │
│                                                     │
│ ┌─────────────────────────────────────────────┐   │
│ │ Lightning Core                               │   │
│ │ • Channel state machines                     │   │
│ │ • HTLC enforcement                           │   │
│ │ • Gossip protocol                            │   │
│ │ • Onion routing                              │   │
│ │ • Watchtowers                                │   │
│ │ • ZKP crypto (secp256k1-zkp)                 │   │
│ └─────────────────────────────────────────────┘   │
│                                                     │
│ ┌─────────────────────────────────────────────┐   │
│ │ gRPC Clients (dinerod boundary)              │   │
│ │ • BlockchainClient                           │   │
│ │ • MempoolClient                              │   │
│ │ • WalletClient                               │   │
│ └─────────────────────────────────────────────┘   │
│                                                     │
│ ┌─────────────────────────────────────────────┐   │
│ │ Lightning Database (RocksDB)                 │   │
│ │ • Channel state                              │   │
│ │ • HTLC tracking                              │   │
│ │ • Routing graph                              │   │
│ │ • Watchtower backups                         │   │
│ └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                        ⬇ gRPC (TCP/Unix socket)
┌─────────────────────────────────────────────────────┐
│ dinerod (L1 Process)                                │
│                                                     │
│ ┌─────────────────────────────────────────────┐   │
│ │ gRPC Server                                  │   │
│ │ • BlockchainService (chain queries)          │   │
│ │ • MempoolService (tx broadcast)              │   │
│ │ • WalletService (signing, UTXOs)             │   │
│ └─────────────────────────────────────────────┘   │
│                                                     │
│ ┌─────────────────────────────────────────────┐   │
│ │ Consensus Engine                             │   │
│ │ • Block validation                           │   │
│ │ • UTXO set                                   │   │
│ │ • Mempool                                    │   │
│ │ • P2P network                                │   │
│ └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

---

## CMake Target Definition (Post-Phase 3)

### Current State (Phase 2)
```cmake
# lightningd doesn't exist yet - Lightning embedded in dinerod
add_executable(dinerod ...)
target_link_libraries(dinerod PRIVATE
    dinero_core           # Includes Lightning via conditional
    dinero_wallet         # ⚠️ Lightning still linked
    ...
)
```

### Target State (Phase 3 Complete)
```cmake
# ═══════════════════════════════════════════════════════════════
# lightningd Binary (Pure L2 Daemon)
# ═══════════════════════════════════════════════════════════════

add_executable(lightningd
    # Main entry point
    src/lightningd/main.cpp                    # Lightning daemon lifecycle
    src/lightningd/lightning_daemon_app.cpp    # LightningDaemonApp (like DaemonApp)

    # gRPC Client wrappers (communicate with dinerod)
    src/lightning/wallet_client.cpp            # WalletClient (gRPC → WalletService)
    src/lightning/blockchain_client_wrapper.cpp  # BlockchainClient wrapper
    src/lightning/mempool_client_wrapper.cpp     # MempoolClient wrapper

    # Wallet/Consensus stubs (should NEVER be called at runtime)
    src/lightningd/wallet_manager_stub.cpp     # WalletManager::listUnspentUTXOs() stub
    src/lightningd/hd_wallet_stub.cpp          # HDWallet::GetLightning*KeyAt() stubs

    # Lightning core logic (L2 only)
    src/lightning/lightning_service.cpp        # LightningService
    src/lightning/channel_manager.cpp          # Channel state machines
    src/lightning/htlc_manager.cpp             # HTLC enforcement
    src/lightning/payment_router.cpp           # Onion routing
    src/lightning/commitment_builder.cpp       # Commitment transaction construction
    src/lightning/watchtower_client.cpp        # Watchtower integration
    src/lightning/lightning_event_manager.cpp  # Event processing
    src/lightning/lightning_peer.cpp           # P2P Lightning protocol
    src/lightning/invoice.cpp                  # BOLT-11 invoice handling
    src/lightning/onion.cpp                    # Onion routing crypto

    # Lightning database
    src/lightning/sqlite_lightning_db.cpp      # Lightning channel DB

    # Lightning RPC server (separate from dinerod RPC)
    src/lightningd/lightning_rpc_server.cpp    # Lightning-specific RPC server
    src/lightningd/lightning_rpc_handlers.cpp  # RPC method handlers
)

# ═══════════════════════════════════════════════════════════════
# Link Libraries (MINIMAL - No Consensus or Wallet!)
# ═══════════════════════════════════════════════════════════════

target_link_libraries(lightningd PRIVATE
    # gRPC protocol definitions (ONLY interface, no implementation)
    dinerod_proto                 # ✅ WalletService, BlockchainService, MempoolService

    # Transaction primitives (NO wallet logic)
    dinero_tx_primitives          # ✅ Transaction::Serialize(), TxInput, TxOutput

    # Lightning-specific libraries
    dinero_lightning              # ✅ Lightning core logic (conditionally built)
    lightning_core_static         # ✅ secp256k1-zkp, BOLT crypto

    # Database
    RocksDB::rocksdb              # ✅ Channel state persistence
    sqlite3                       # ✅ Lightning database

    # Crypto (shared primitives only)
    OpenSSL::Crypto               # ✅ SHA256, RIPEMD160, ECDSA
    secp256k1                     # ✅ Signature verification

    # Networking
    pthread                       # ✅ Thread support
    ${CMAKE_DL_LIBS}              # ✅ Dynamic linking

    # JSON
    jsoncpp_static                # ✅ RPC serialization

    # ❌ NO dinero_wallet
    # ❌ NO dinero_consensus
    # ❌ NO dinero_chainstate
    # ❌ NO dinero_core
)

# ═══════════════════════════════════════════════════════════════
# Include Paths (Minimal - No Wallet Headers!)
# ═══════════════════════════════════════════════════════════════

target_include_directories(lightningd PRIVATE
    ${CMAKE_SOURCE_DIR}/include/lightning      # ✅ Lightning headers
    ${CMAKE_SOURCE_DIR}/include/primitives     # ✅ Transaction primitives
    ${CMAKE_SOURCE_DIR}/build/generated/proto  # ✅ gRPC proto headers
    ${CMAKE_SOURCE_DIR}/third_party/lightning_core/include  # ✅ BOLT crypto
)
```

---

## Dependency Graph (Post-Phase 3)

### Before Phase 3 (Current - Embedded Lightning)
```
dinerod
  └── dinero_core
       ├── dinero_wallet ⚠️ (includes Lightning)
       ├── dinero_consensus
       ├── dinero_chainstate
       └── dinero_lightning (if ENABLE_LIGHTNING=ON)
```

### After Phase 3 (Standalone Lightning)
```
dinerod (L1 Node)
  ├── dinero_core
  │    ├── dinero_wallet
  │    ├── dinero_consensus
  │    └── dinero_chainstate
  └── dinerod_proto (gRPC server)

lightningd (L2 Node) ← SEPARATE BINARY
  ├── dinerod_proto (gRPC client) ✅ ONLY connection to dinerod
  ├── dinero_tx_primitives (Transaction types)
  ├── dinero_lightning (L2 logic)
  └── lightning_core_static (BOLT crypto)
```

---

## Symbol Verification Script

After Phase 3, verify lightningd independence:

```bash
#!/bin/bash
# scripts/verify_lightningd_independence.sh

FORBIDDEN_SYMBOLS=(
    # Consensus symbols (should NOT be in lightningd)
    "BlockValidator::"
    "ValidateBlock"
    "VerifyScript"
    "CheckProofOfWork"
    "ActivateBestChain"

    # Wallet symbols (should NOT be in lightningd except stubs)
    "WalletManager::createTransaction"
    "WalletManager::signTransaction"
    "HDWallet::Unlock"
    "UTXOIndex::AddUTXO"
    "UTXOIndex::SpendUTXO"

    # Chainstate symbols (should NOT be in lightningd)
    "ChainDB::getBlock"
    "ChainDB::setTip"
    "Mempool::addTransaction"
)

ALLOWED_SYMBOLS=(
    # gRPC client symbols (OK - these are client-side only)
    "WalletClient::"
    "BlockchainClient::"
    "MempoolClient::"

    # Transaction primitives (OK - stateless)
    "Transaction::Serialize"
    "TxInput::"
    "TxOutput::"

    # Lightning symbols (OK - L2 logic)
    "ChannelManager::"
    "HTLCManager::"
    "LightningService::"
)

# Check for forbidden symbols
echo "🔍 Checking lightningd binary for forbidden symbols..."
FOUND_FORBIDDEN=0

for sym in "${FORBIDDEN_SYMBOLS[@]}"; do
    if nm build/bin/lightningd | grep -q "$sym"; then
        echo "❌ FOUND FORBIDDEN SYMBOL: $sym"
        FOUND_FORBIDDEN=1
    fi
done

if [ $FOUND_FORBIDDEN -eq 0 ]; then
    echo "✅ No forbidden symbols found in lightningd"
else
    echo "❌ lightningd contains consensus/wallet symbols - Phase 3 incomplete"
    exit 1
fi

# Verify allowed symbols are present
echo ""
echo "🔍 Verifying required symbols present..."
FOUND_REQUIRED=0

for sym in "${ALLOWED_SYMBOLS[@]}"; do
    if nm build/bin/lightningd | grep -q "$sym"; then
        FOUND_REQUIRED=$((FOUND_REQUIRED + 1))
    fi
done

if [ $FOUND_REQUIRED -gt 0 ]; then
    echo "✅ Found $FOUND_REQUIRED required symbol groups"
else
    echo "⚠️  Warning: Some required symbols may be missing"
fi

echo ""
echo "✅ lightningd binary is properly decoupled from consensus/wallet"
exit 0
```

---

## Binary Size Comparison

### Before Phase 3
```bash
# dinerod includes everything (L1 + L2)
$ ls -lh build/bin/dinerod
-rwxr-xr-x  1 user  staff   45M  Jan  7 12:00 dinerod

# No lightningd binary yet
```

### After Phase 3
```bash
# dinerod (L1 only - no Lightning)
$ ls -lh build/bin/dinerod
-rwxr-xr-x  1 user  staff   38M  Jan  7 18:00 dinerod  # ✅ ~7MB smaller

# lightningd (L2 only - no consensus)
$ ls -lh build/bin/lightningd
-rwxr-xr-x  1 user  staff   12M  Jan  7 18:00 lightningd  # ✅ New binary

# Total: 50MB (5MB larger, but properly separated)
```

**Trade-off:** Slightly larger total size, but:
- Clean separation of concerns
- Independent deployment
- Can run on different machines
- External Lightning implementations possible

---

## Runtime Configuration

### Start dinerod (L1 Node)
```bash
# Start dinerod with gRPC server enabled
$ dinerod --grpc-server --grpc-port=50051 --rpcuser=user --rpcpassword=pass
```

### Start lightningd (L2 Node)
```bash
# Connect lightningd to dinerod via gRPC
$ lightningd --dinerod-host=localhost --dinerod-port=50051 \
             --grpc-user=user --grpc-password=pass \
             --lightning-port=9735
```

### Configuration File (lightningd.conf)
```ini
# lightningd configuration
[grpc]
dinerod_host = localhost
dinerod_port = 50051
grpc_user = user
grpc_password = pass

[lightning]
listen_port = 9735
data_dir = ~/.lightning
max_channels = 100
max_htlc_value = 1000000
```

---

## External Lightning Integration Example

After Phase 3, third parties can build custom Lightning implementations:

```bash
# Clone DineroCoin gRPC protocol definitions only
$ git clone https://github.com/dinerocoin/grpc-proto.git
$ cd grpc-proto

# Generate gRPC stubs in any language
$ protoc --cpp_out=. dinerod.proto      # C++
$ protoc --python_out=. dinerod.proto   # Python
$ protoc --go_out=. dinerod.proto       # Go

# Implement custom Lightning using WalletService
# NO DineroCoin libraries required!
```

**Example Custom Implementation (Python):**
```python
import grpc
from dinerod_pb2 import *
from dinerod_pb2_grpc import WalletServiceStub

# Connect to dinerod
channel = grpc.insecure_channel('localhost:50051')
wallet = WalletServiceStub(channel)

# Use wallet operations
utxos = wallet.ListUnspentUTXOs(ListUnspentUTXOsRequest(
    min_confirmations=1
))

# Build custom Lightning implementation
# ... your custom L2 protocol here ...
```

---

## Phase 3 Completion Criteria

**lightningd is ready when:**
- [ ] `lightningd` binary compiles independently
- [ ] `lightningd` does NOT link `dinero_wallet`
- [ ] `lightningd` does NOT link `dinero_consensus`
- [ ] `lightningd` does NOT link `dinero_chainstate`
- [ ] All wallet access goes through `WalletClient` (gRPC)
- [ ] Symbol verification script passes
- [ ] Binary size reduction confirmed (~5-10 MB for `dinerod`)
- [ ] `lightningd` can connect to `dinerod` via gRPC
- [ ] Channel lifecycle tests pass (open, update, close)

---

## Next Steps After Phase 3

1. **Process Separation** (Option B from original plan)
   - Run `dinerod` and `lightningd` as separate processes
   - Test cross-process gRPC communication
   - Implement automatic restart on connection loss

2. **External Lightning Documentation**
   - Publish gRPC API documentation
   - Create integration guide for third-party implementations
   - Provide example implementations (Python, Go, Rust)

3. **Performance Optimization**
   - gRPC connection pooling
   - Batch RPC calls where possible
   - Optimize channel database queries

---

**Document Status:** Planned (Post-Phase 3)
**Owner:** DineroCoin Core Team
**Review Cycle:** Before Phase 3 execution starts
