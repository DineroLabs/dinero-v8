# Dinero Mining Architecture

## One-Line Summary

> **Nodes validate, Stratum mediates, external miners hash.**

Mixing these roles is always a mistake.

## Three-Miner Architecture

Dinero now has **three** mining implementations with distinct purposes:

| Miner | Location | Purpose | Users |
|-------|----------|---------|-------|
| **Internal Miner** | `dinerod` | Testing only | Developers |
| **External RPC Miner** | `dinero-miner` | Solo production mining | End users |
| **Stratum Worker** | `dinero-stratum-worker` | Pool/Stratum mining | End users |

### Why Three Miners?

This mirrors Bitcoin Core's evolution:
- Bitcoin kept internal mining for testing
- Removed it from GUI
- Never treated it as a user miner
- Production mining uses external tools (cgminer, bfgminer, etc.)

## Internal Miner (TESTING ONLY)

**Location:** `src/internal_miner/` and RPC methods

**Purpose:**
- Consensus testing
- ASERT / DAA testing
- Reorg testing
- CI / regtest
- Developer convenience

**Characteristics:**
- RPC-based (`mining.start`, `mining.stop`)
- Not optimized
- Not user-facing
- Can be slow
- Can be unsafe for exposure

**Status:**
- 🔒 **Disabled or hidden in production**
- 🔒 Never exposed publicly
- 🔒 Never documented for users
- Only enabled via `ENABLE_INTERNAL_MINER` compile flag or `-regtest`/`-testnet`

**RPC Methods (testing only):**
```
mining.start [threads]      - Start internal miner
mining.stop                 - Stop internal miner
mining.getstatus            - Get mining status
mining.setthreads [n]       - Adjust thread count
mining.setaddress [addr]    - Set mining address
```

**When to use:**
```bash
# Regtest testing
./dinerod -regtest -rpcport=20996
./dinero-cli -regtest mining.start 1
./dinero-cli -regtest generatetoaddress 1 din1...

# CI testing
cmake -DENABLE_INTERNAL_MINER=ON ...
```

## External RPC Miner (PRODUCTION SOLO)

**Location:** `tools/dinero_miner.cpp`

**Purpose:**
- User mining
- Production mining
- End-to-end testing
- CPU-only phase participation

**Characteristics:**
- Standalone binary
- Speaks RPC to `dinerod`
- Optimized hashing loop
- User-friendly CLI
- Cross-platform
- Kept separate from pool mining on purpose

**Status:**
- ✅ Public
- ✅ Documented
- ✅ Versioned
- ✅ Stable interface

**Usage:**
```bash
# Connect directly to local node for solo mining
./dinero-miner --rpc http://127.0.0.1:20998/ --address=din1... --threads=8
```

## Stratum Worker (PRODUCTION POOL)

**Location:** `tools/dinero_stratum_worker.cpp`

**Purpose:**
- Pool mining
- Stratum client connectivity
- End-to-end miner → stratum → daemon validation

**Characteristics:**
- Standalone binary
- Speaks Stratum directly to `dinero-stratum`
- CPU-only worker
- Leaves the solo RPC miner untouched

**Usage:**
```bash
# Connect to local stratum server
./dinero-stratum-worker --stratum=localhost:3333 --user=din1... --threads=8

# Connect to pool with worker suffix
./dinero-stratum-worker --stratum=pool.dinero-coin.com:3333 --user=din1....worker1 --password=x
```

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        PRODUCTION PATH                          │
│                                                                 │
│  ┌──────────────┐    Stratum    ┌────────────────┐             │
│  │dinero-stratum│◄─────────────►│ dinero-stratum │             │
│  │  -worker     │               │   (pool/solo)  │             │
│  └──────────────┘               └───────┬────────┘             │
│                                         │ RPC                   │
│                                         ▼                       │
│                                 ┌──────────────┐               │
│                                 │   dinerod    │               │
│                                 │  (consensus) │               │
│                                 └──────────────┘               │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                        TESTING PATH                             │
│                                                                 │
│  ┌──────────────────────────────────────────┐                  │
│  │              dinerod -regtest            │                  │
│  │  ┌────────────────┐  ┌────────────────┐  │                  │
│  │  │ Internal Miner │  │   Consensus    │  │                  │
│  │  │  (RPC-based)   │  │                │  │                  │
│  │  └────────────────┘  └────────────────┘  │                  │
│  └──────────────────────────────────────────┘                  │
│                                                                 │
│  Used only for: regtest, CI, consensus tests                   │
└─────────────────────────────────────────────────────────────────┘
```

## Why This Separation Matters

### If you DON'T separate:
- ❌ Tempted to expose node RPC mining to users
- ❌ Blur production vs testing paths
- ❌ Tie mining UX to consensus code
- ❌ Make Stratum harder to test
- ❌ Block future GPU/ASIC miners

### With separation:
- ✅ Production path uses Stratum (real protocol)
- ✅ Testing path is isolated
- ✅ Can swap miners later (GPU, ASIC)
- ✅ Clean mental model for users

## User Documentation

**For users who want to mine:**

> "To mine Dinero, download `dinero-miner` and connect to a Stratum server (or run your own `dinero-stratum`)."

**NOT:**

> "Compile the node, enable flags, open RPC ports..."

## Repository Layout

```
~/src/
├── dinero/              # Consensus daemon
│   └── src/internal_miner/   # TEST ONLY (gated)
│
├── dinero-stratum/      # Stratum server
│   └── src/...
│
├── dinero-miner/        # Production miner (Stratum client)
│   ├── src/
│   │   ├── stratum_client.cpp
│   │   ├── cpu_hash.cpp
│   │   └── main.cpp
│   └── CMakeLists.txt
│
└── (future)
    ├── dinero-gpu-miner/
    └── dinero-asic-miner/
```

## Compile Flags

### Enable internal miner (for testing):
```bash
cmake -DENABLE_INTERNAL_MINER=ON -DCMAKE_BUILD_TYPE=Debug ...
```

### Production build (internal miner disabled):
```bash
cmake -DENABLE_INTERNAL_MINER=OFF -DCMAKE_BUILD_TYPE=Release ...
```

## Summary

| Component | Role | Protocol | User-Facing |
|-----------|------|----------|-------------|
| `dinerod` | Consensus | P2P | No (node operators) |
| `dinero-stratum` | Mining pool | Stratum V1 | No (pool operators) |
| `dinero-miner` | Hashing | Stratum V1 | **Yes** |
| Internal miner | Testing | RPC | **Never** |

This is the same architecture Bitcoin uses, and you're making this decision **before** production — which is exactly when it should be made.
