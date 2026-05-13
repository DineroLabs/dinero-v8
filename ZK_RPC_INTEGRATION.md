# ZK RPC Integration - Step-by-Step Guide

**Status:** Ready for integration when Phase B complete
**File:** `src/rpc/zk_rpc_handlers_context.cpp` (skeleton created, awaiting implementation)

---

## Integration Checklist

When Phase B (Bulletproof range proofs) is complete and you're ready to activate the ZK RPC methods:

- [ ] Implement RPC method bodies in `src/rpc/zk_rpc_handlers_context.cpp`
- [ ] Add ZK RPC file to CMakeLists.txt
- [ ] Register ZK RPC methods in rpc_context_wiring.cpp
- [ ] Test with dinero-cli
- [ ] Document usage examples

---

## Step 1: Add to CMakeLists.txt

**File:** `/Users/haydarevich/Documents/DineroCoin/CMakeLists.txt`
**Location:** Around line 442 (after logs_rpc_handlers_context.cpp)

```cmake
  src/rpc/logging_rpc_handlers_context.cpp  # logging.setlevel, logging.getlevel (Step C: Dynamic log control)
  src/rpc/logs_rpc_handlers_context.cpp  # logs.recent, logs.services, logs.tail (Unified log aggregator)
  src/rpc/zk_rpc_handlers_context.cpp  # zk.createtx, zk.verify, zk.scanviewkey (Zero-knowledge privacy)  # ← ADD THIS LINE
  # Week 9: GPU Mining RPC handlers (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
```

**Exact change:**
```diff
  src/rpc/logging_rpc_handlers_context.cpp  # logging.setlevel, logging.getlevel (Step C: Dynamic log control)
  src/rpc/logs_rpc_handlers_context.cpp  # logs.recent, logs.services, logs.tail (Unified log aggregator)
+ src/rpc/zk_rpc_handlers_context.cpp  # zk.createtx, zk.verify, zk.scanviewkey (Zero-knowledge privacy)
  # Week 9: GPU Mining RPC handlers (mining.gpustatus, mining.allowgpu, mining.gpuinfo)
```

---

## Step 2: Register in rpc_context_wiring.cpp

**File:** `/Users/haydarevich/Documents/DineroCoin/src/daemon/rpc_context_wiring.cpp`

### 2a. Add Forward Declaration (around line 33)

```cpp
void WireDiagnosticsRpcContext();  // Node diagnostics (node.info, rpc.methods)
void WireLoggingRpcContext();  // Logging control (logging.setlevel, logging.getlevel)
void WireLogsRpcContext();  // Log aggregation (logs.recent, logs.services, logs.tail)
void WireZkRpcContext();  // Zero-knowledge privacy (zk.createtx, zk.verify, zk.scanviewkey)  // ← ADD THIS
```

**Exact change:**
```diff
void WireDiagnosticsRpcContext();  // Node diagnostics (node.info, rpc.methods)
void WireLoggingRpcContext();  // Logging control (logging.setlevel, logging.getlevel)
void WireLogsRpcContext();  // Log aggregation (logs.recent, logs.services, logs.tail)
+void WireZkRpcContext();  // Zero-knowledge privacy (zk.createtx, zk.verify, zk.scanviewkey)
```

### 2b. Register Methods (around line 209)

```cpp
        // Log aggregation namespace (logs.recent, logs.services, logs.tail) - Unified log aggregator
        WireLogsRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Log aggregation context-aware handlers registered");

        // Zero-knowledge privacy namespace (zk.createtx, zk.verify, zk.scanviewkey)  // ← ADD THIS
        WireZkRpcContext();  // ← ADD THIS
        dinero::g_logger.info("[RPC Context] ✅ ZK privacy context-aware handlers registered");  // ← ADD THIS

    } catch (const std::exception& e) {
```

**Exact change:**
```diff
        // Log aggregation namespace (logs.recent, logs.services, logs.tail) - Unified log aggregator
        WireLogsRpcContext();
        dinero::g_logger.info("[RPC Context] ✅ Log aggregation context-aware handlers registered");

+       // Zero-knowledge privacy namespace (zk.createtx, zk.verify, zk.scanviewkey)
+       WireZkRpcContext();
+       dinero::g_logger.info("[RPC Context] ✅ ZK privacy context-aware handlers registered");

    } catch (const std::exception& e) {
```

---

## Step 3: Uncomment Includes in zk_rpc_handlers_context.cpp

**File:** `/Users/haydarevich/Documents/DineroCoin/src/rpc/zk_rpc_handlers_context.cpp`
**Lines 22-26:**

When you have implemented the ZK library code, uncomment these includes:

```cpp
// TODO: Add these includes when ZK code is implemented
#include "zk/confidential_tx.h"       // ← Uncomment when Phase A/B complete
#include "zk/zk_validation.h"         // ← Uncomment when Phase A/B complete
#include <secp256k1.h>                // ← Uncomment when Phase A/B complete
#include <secp256k1_generator.h>      // ← Uncomment when Phase A/B complete
#include <secp256k1_rangeproof.h>     // ← Uncomment when Phase B complete
```

---

## Step 4: Rebuild the Project

```bash
cd /Users/haydarevich/Documents/DineroCoin/build
cmake --build . --target dinerod
```

**Expected output:**
```
[100%] Building CXX object CMakeFiles/dinerod.dir/src/rpc/zk_rpc_handlers_context.cpp.o
[100%] Linking CXX executable dinerod
[100%] Built target dinerod
```

**Startup verification:**
```bash
./dinerod --datadir=/tmp/test-zk
```

**Look for:**
```
[RPC Context] ✅ ZK privacy context-aware handlers registered
```

---

## Step 5: Test RPC Methods

### Verify Registration

```bash
dinero-cli rpc.methods | grep zk
```

**Expected output:**
```json
[
  "zk.createtx",
  "zk.verify",
  "zk.verifyrangeproof",
  "zk.scanviewkey",
  "zk.getcommitment"
]
```

### Test Individual Methods

**Phase A: Verify commitment balance (optional, for testing)**
```bash
dinero-cli zk.verify '{"txhex": "010000..."}'
```

**Phase B: Create confidential transaction (CRITICAL)**
```bash
dinero-cli zk.createtx '{
  "inputs": [
    {"txid": "abc123...", "vout": 0, "amount": 100000000, "blinding_factor": "hex..."}
  ],
  "outputs": [
    {"address": "dinero1q...", "amount": 50000000, "confidential": true}
  ],
  "fee": 10000
}'
```

**Expected output:**
```json
{
  "hex": "010000...",
  "txid": "def456...",
  "commitments": [
    {
      "vout": 0,
      "commitment": "hex...",
      "blinding_factor": "hex...",
      "nonce": "hex...",
      "rangeproof_size": 5134
    }
  ],
  "verify": {
    "balance": true,
    "range_proofs": true
  }
}
```

**Phase C: Scan with view key (for receivers)**
```bash
dinero-cli zk.scanviewkey '{
  "viewkey": "hex...",
  "start_height": 0,
  "end_height": 1000
}'
```

---

## Integration Status

### ✅ Already Complete (No Action Needed)

- [x] ZK library infrastructure in CMakeLists.txt (lines 164-204)
  - `dinero_zk` library already configured
  - Links against `secp256k1-zkp` for Pedersen commitments + Bulletproofs
  - Links against `libwally-core` for PSBT wrapper
  - Defines `HAVE_ZK_PRIVACY` and `HAVE_CONFIDENTIAL_TX`

- [x] RPC skeleton file created (`src/rpc/zk_rpc_handlers_context.cpp`)
  - All 5 methods stubbed: `zk.createtx`, `zk.verify`, `zk.verifyrangeproof`, `zk.scanviewkey`, `zk.getcommitment`
  - Registration function `WireZkRpcContext()` implemented
  - Follows DineroCoin's context-aware RPC pattern

- [x] Documentation created
  - `ZK_API_CHEATSHEET.md` - Phase A API reference
  - `PHASE_B_RANGE_PROOFS.md` - Phase B implementation guide
  - `ZK_RPC_IMPLEMENTATION_GUIDE.md` - RPC usage guide
  - `ZK_LIBRARIES_STATUS.md` - Library status (all vendored, ready!)

### ⏳ Awaiting Phase A/B Completion (User Working On This)

- [ ] **Phase A:** Pedersen commitment implementation in `src/zk/confidential_tx.cpp`
  - User is currently implementing this
  - Once complete, `zk.verify` becomes functional (optional)

- [ ] **Phase B:** Bulletproof range proof implementation
  - Add range proof generation and verification
  - Once complete, `zk.createtx` becomes functional (CRITICAL)

### 📦 Ready for Integration (When Phase B Complete)

- [ ] Add `src/rpc/zk_rpc_handlers_context.cpp` to CMakeLists.txt
- [ ] Add forward declaration to rpc_context_wiring.cpp
- [ ] Register `WireZkRpcContext()` in rpc_context_wiring.cpp
- [ ] Uncomment includes in zk_rpc_handlers_context.cpp
- [ ] Implement RPC method bodies (replace `throw` statements with actual logic)
- [ ] Rebuild and test

---

## Implementation Priority

**Immediate (User working on this):**
- Phase A: Core Pedersen commitment library

**Next (When Phase A complete):**
- Phase B: Bulletproof range proofs
- Implement `zk.createtx` RPC method (MOST IMPORTANT)

**Later (Phase C):**
- Implement `zk.scanviewkey` for receivers
- Add stealth address support

---

## Quick Reference: File Locations

```
/Users/haydarevich/Documents/DineroCoin/
├── CMakeLists.txt                          # Add RPC file around line 442
├── src/
│   ├── daemon/
│   │   └── rpc_context_wiring.cpp          # Add forward decl + registration
│   ├── rpc/
│   │   └── zk_rpc_handlers_context.cpp     # Skeleton (implement methods)
│   └── zk/
│       └── confidential_tx.cpp             # Phase A/B implementation (user working)
└── docs/
    ├── ZK_API_CHEATSHEET.md                # Phase A API reference
    ├── PHASE_B_RANGE_PROOFS.md             # Phase B implementation guide
    ├── ZK_RPC_IMPLEMENTATION_GUIDE.md      # RPC usage guide
    ├── ZK_RPC_INTEGRATION.md               # This file
    └── ZK_LIBRARIES_STATUS.md              # Library status
```

---

## Notes

- **DO NOT integrate yet** - Wait until Phase A/B implementation is complete
- The skeleton file is intentionally not added to the build to avoid interference
- All integration points documented and ready for seamless activation
- User is currently implementing Phase A independently

**When ready to integrate:** Follow Steps 1-5 above in order.
