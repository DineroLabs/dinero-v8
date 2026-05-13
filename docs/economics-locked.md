# Dinero Economics - Locked-In Parameters

## Block Time & Retargeting
- **Target Block Time**: 520 seconds (8.7 minutes)
- **Retarget Interval**: 60 blocks (~8.7 hours)
- **Coinbase Maturity**: 100 blocks (~14.4 hours)

## Supply Schedule

### Phase 1: Developer Fund (Premine)
- **Amount**: 7,000,000 DIN
- **Duration**: Genesis block only
- **Reward**: 0 DIN per block (premined)

### Phase 2: CPU-Friendly Mining
- **Supply Range**: 7M → 25M DIN
- **Reward**: 99 DIN per block
- **Duration**: ~3.0 years (181,818 blocks)
- **Purpose**: Accessible CPU mining for community building

### Phase 3: Halving Schedule
- **Supply Range**: 25M → 180.5M DIN
- **Reward Schedule**: 99 → 66 → 33 → 16 → 8 → 4 → 2 → 1 DIN per block
- **Epoch Length**: 242,584 blocks (~4 years each)
- **Transition**: Supply-based (not height-based)

### Phase 4: Tail Emission
- **Supply Range**: 180.5M+ DIN
- **Reward**: 1 DIN per block (forever)
- **Purpose**: Network security and miner incentives

## Total Supply
- **Pre-Tail**: 180,500,000 DIN
- **Tail Emission**: 1 DIN per block indefinitely
- **Economics**: Deflationary until 180.5M, then stable inflation

## Port Semantics
- **Core RPC Port**: Internal RPC communication (controlled by `-rpcport`)
- **HTTP JSON-RPC Port**: External HTTP API for tools/CLI (default 20999)
- **Separate Ports Mode**: Daemon runs core RPC and HTTP JSON-RPC on different ports
- **Port Discovery**: Parse daemon logs for "Separate ports mode: RPC=X HTTP=Y" to find HTTP port
- **Mainnet HRP**: `din`
- **Testnet HRP**: `tdin`
- **Regtest HRP**: `rdin`

## Consensus Rules
- **Difficulty Algorithm**: ASERT (Adaptive Difficulty Adjustment)
- **Block Size**: Variable (segwit-compatible)
- **Transaction Format**: Bitcoin-compatible with Dinero-specific features

## Lock-In Status
These parameters are **permanently locked** and cannot be changed without a hard fork. The system includes multiple regression prevention mechanisms:

1. **Unit Tests**: Verify all constants and calculations
2. **Runtime Banner**: Daemon prints locked economics at startup
3. **CLI Help**: Shows Network Info section with current parameters
4. **Smoke Tests**: End-to-end validation of block time and rewards

## Upgrade Notes
- **Regtest/Testnet**: Remove old datadirs after upgrading due to consensus changes
- **Port Semantics**: Daemon uses separate ports for core RPC vs HTTP JSON-RPC
- **Tooling**: Parse daemon logs for "Separate ports mode: RPC=X HTTP=Y" to detect HTTP port
