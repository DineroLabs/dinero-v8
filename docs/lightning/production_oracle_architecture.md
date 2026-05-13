# Production Oracle Architecture (Phase 8.7)

## Problem Statement

lightningd (L2) uses mock oracles that return fake data. For production use, it needs access to real L1 state (blockchain, wallet, mempool) without violating Phase 8 architectural separation.

**Phase 8 Invariants (MUST preserve):**
- ❌ lightningd CANNOT link against L1 code (daemon/chainstate/wallet)
- ❌ lightningd CANNOT include L1 headers
- ✅ lightningd MUST communicate with L1 via IPC only

## Solution: IPC-Based Oracle Bridge

### Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                    dinerod (L1)                     │
│  ┌─────────────────────────────────────────────┐   │
│  │      ProductionChainOracle                  │   │
│  │  - Wraps Chainstate/ChainstateManager       │   │
│  │  - getBlockHeight() → chainstate.height()   │   │
│  │  - isUnspent() → UTXO set lookup            │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │      ProductionWalletOracle                 │   │
│  │  - Wraps IWalletAPI                         │   │
│  │  - getConfirmedBalance() → wallet balance   │   │
│  │  - isAvailable() → wallet loaded?           │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │      ProductionFundingService               │   │
│  │  - Uses IWalletAPI + TaprootTxSigner        │   │
│  │  - createFunding() → build funding TX       │   │
│  └─────────────────────────────────────────────┘   │
│                      ↓ IPC Protocol                 │
│              Unix Socket: /tmp/lightningd.sock      │
└─────────────────────────────────────────────────────┘
                       ↓
┌─────────────────────────────────────────────────────┐
│                 lightningd (L2)                     │
│  ┌─────────────────────────────────────────────┐   │
│  │      IPCChainOracle                         │   │
│  │  - IPC client wrapper                       │   │
│  │  - getBlockHeight() → IPC request           │   │
│  │  - Implements IChainOracle interface        │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │      IPCWalletOracle                        │   │
│  │  - IPC client wrapper                       │   │
│  │  - Implements IWalletOracle interface       │   │
│  └─────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────┐   │
│  │      IPCFundingService                      │   │
│  │  - IPC client wrapper                       │   │
│  │  - Implements IFundingService interface     │   │
│  └─────────────────────────────────────────────┘   │
│                                                     │
│  LightningApp uses IPC oracles instead of mocks    │
└─────────────────────────────────────────────────────┘
```

## IPC Oracle Protocol

### Wire Format

Text-based protocol extending existing IPC format:

**Request Format:**
```
ORACLE_<CATEGORY>_<METHOD> param1=value1 param2=value2
```

**Response Format:**
```
ORACLE_<CATEGORY>_RESULT status=ok|error result=<data> [error=<msg>]
```

### Chain Oracle Protocol

#### getBlockHeight()
```
Request:  ORACLE_CHAIN_GET_HEIGHT
Response: ORACLE_CHAIN_RESULT status=ok height=850000
```

#### isUnspent(txid, vout)
```
Request:  ORACLE_CHAIN_IS_UNSPENT txid=abc123... vout=0
Response: ORACLE_CHAIN_RESULT status=ok unspent=true
```

#### getBlockHash(height)
```
Request:  ORACLE_CHAIN_GET_BLOCK_HASH height=850000
Response: ORACLE_CHAIN_RESULT status=ok hash=def456...
```

### Wallet Oracle Protocol

#### getConfirmedBalance()
```
Request:  ORACLE_WALLET_GET_BALANCE
Response: ORACLE_WALLET_RESULT status=ok balance=1000000000
```

#### isAvailable()
```
Request:  ORACLE_WALLET_IS_AVAILABLE
Response: ORACLE_WALLET_RESULT status=ok available=true
```

### Funding Service Protocol

#### createFunding(amount, remote_pubkey, local_pubkey, csv_delay, feerate)
```
Request:  ORACLE_FUNDING_CREATE amount=1000000 remote_pubkey=02abc... local_pubkey=03def... csv_delay=144 feerate=10
Response: ORACLE_FUNDING_RESULT status=ok funding_txid=ghi789... funding_vout=0 funding_amount=1000000 funding_tx_hex=020000...
```

**Error Response:**
```
Response: ORACLE_FUNDING_RESULT status=error error="Insufficient balance"
```

## Implementation Plan

### Phase 1: L1 Side (dinerod) - Production Oracle Implementations

**Files to Create:**
1. `src/lightning/production_chain_oracle.h/cpp`
   - Wraps Chainstate/ChainstateManager
   - Implements IChainOracle interface
   - Direct access to L1 UTXO set, block index

2. `src/lightning/production_wallet_oracle.h/cpp`
   - Wraps IWalletAPI
   - Implements IWalletOracle interface
   - Queries wallet balance, availability

3. `src/lightning/production_funding_service.h/cpp`
   - Uses IWalletAPI + TaprootTxSigner
   - Implements IFundingService interface
   - Builds and signs 2-of-2 multisig funding transactions

**Integration Point:**
- Instantiate production oracles in daemon startup
- Pass to LightningService (which talks to lightningd via IPC)

### Phase 2: IPC Protocol Extension

**Files to Modify:**
1. `src/lightningd/ipc_server.cpp`
   - Add oracle request handlers
   - Parse `ORACLE_*` commands
   - Call production oracles
   - Serialize responses

2. `src/lightningd/ipc_server.h`
   - Add oracle method routing
   - Store references to production oracles

### Phase 3: L2 Side (lightningd) - IPC Oracle Clients

**Files to Create:**
1. `include/lightning/ipc_chain_oracle.h`
   - IPC client wrapper
   - Implements IChainOracle interface
   - Sends IPC requests, parses responses

2. `src/lightning/ipc_chain_oracle.cpp`
   - Socket communication
   - Request serialization
   - Response parsing

3. `include/lightning/ipc_wallet_oracle.h/cpp`
   - Similar to chain oracle

4. `include/lightning/ipc_funding_service.h/cpp`
   - Similar pattern

**Integration Point:**
- Update `LightningApp::start()` to use IPC oracles instead of mocks
- Conditional compilation: use mocks in hermetic build, IPC in production

### Phase 4: Testing Strategy

1. **Unit Tests:**
   - Test production oracles with mock L1 services
   - Test IPC oracles with mock socket

2. **Integration Tests:**
   - Start dinerod + lightningd
   - Send oracle requests via IPC
   - Verify responses match L1 state

3. **E2E Tests:**
   - Open channel using production oracles
   - Verify funding TX built correctly
   - Verify channel state persisted

## Build Configuration

### Hermetic Build (lightningd standalone)
```cmake
# Uses mock oracles (NO L1 dependencies)
if(NOT BUILD_DINEROD)
  target_sources(lightningd PRIVATE
    src/lightning/chain_oracle.cpp       # Mock implementations
    src/lightning/wallet_oracle.cpp      # Mock implementations
  )
endif()
```

### Production Build (dinerod + lightningd)
```cmake
# dinerod includes production oracles
if(BUILD_DINEROD)
  target_sources(dinerod PRIVATE
    src/lightning/production_chain_oracle.cpp
    src/lightning/production_wallet_oracle.cpp
    src/lightning/production_funding_service.cpp
  )
endif()

# lightningd includes IPC oracle clients
if(BUILD_LIGHTNINGD)
  target_sources(lightningd PRIVATE
    src/lightning/ipc_chain_oracle.cpp
    src/lightning/ipc_wallet_oracle.cpp
    src/lightning/ipc_funding_service.cpp
  )
endif()
```

## Security Considerations

1. **Input Validation:**
   - Production oracles MUST validate all inputs from IPC
   - Prevent injection attacks via malformed requests

2. **Error Handling:**
   - Never crash on invalid oracle request
   - Return error status, log warning, continue

3. **Resource Limits:**
   - Rate limit oracle requests (prevent DoS)
   - Timeout on slow queries (prevent hang)

4. **Authentication:**
   - Unix socket permissions (owner-only)
   - No authentication needed (local IPC)

## Phase 8.7 Exit Criteria

- ✅ ProductionChainOracle implemented and tested
- ✅ ProductionWalletOracle implemented and tested
- ✅ ProductionFundingService implemented and tested
- ✅ IPC oracle protocol defined and documented
- ✅ IPCChainOracle client working end-to-end
- ✅ IPCWalletOracle client working end-to-end
- ✅ IPCFundingService client working end-to-end
- ✅ LightningApp uses production oracles (not mocks)
- ✅ Hermetic build still passes (uses mocks)
- ✅ Production build passes (uses IPC oracles)
- ✅ E2E test: Open channel using production oracles
