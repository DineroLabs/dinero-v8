# Wallet-Aware Mining RPC Integration (production-ready)

## Overview
This PR lands a canonical, wallet-scoped RPC stack with secure mining address management and full DB persistence.

### Highlights
- **Wallet-scoped URLs**: `/wallet/<name>` context
- **Mining RPCs**: `mining.getaddress`, `mining.setaddress`
- **Wallet RPCs**: `wallet.create`, `wallet.load`, `wallet.getnewaddress`, `wallet.validateaddress`, `wallet.listaddresses`
- **Meta RPCs**: `rpc.capabilities`, `rpc.listmethods`, `rpc.help`, `rpc.health`
- **Persistence**: SQLite schema v3, network-scoped storage for mining address
- **Security**: strict ownership + HRP/network validation
- **Dispatcher**: single JSON-RPC dispatcher; legacy v2 removed
- **Tests**: end-to-end wallet-aware mining tests (8/8 green)

## Breaking changes
- Legacy bare methods (e.g. `getnewaddress`) are replaced by namespaced ones (`wallet.getnewaddress`).  
  If needed, enable aliases via the canonicalizer.

## How to test locally
```bash
# Start daemon (regtest)
DATADIR="$HOME/Library/Application Support/DineroCoin/Dinero All-in-One/data/regtest"
./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=18443 --wsport=18444

# Auth
AUTH="$(tr -d '\r\n' < "$DATADIR/.cookie")"

# Health & capabilities
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"rpc.health"}' http://127.0.0.1:18443/

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"rpc.capabilities"}' http://127.0.0.1:18443/

# Wallet lifecycle
RPC="http://127.0.0.1:18443/wallet/test_wallet"
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"wallet.create","params":{"name":"test_wallet"}}' http://127.0.0.1:18443/
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":4,"method":"wallet.getnewaddress"}' "$RPC"
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":5,"method":"mining.setaddress","params":["<wallet-owned-address>"]}' http://127.0.0.1:18443/
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":6,"method":"mining.getaddress"}' http://127.0.0.1:18443/
```

## Files of interest
- `src/daemon/rpc_server.cpp` (registration + URL scoping)
- `src/daemon/rpc/WalletHandlers.{h,cpp}`
- `src/wallet/wallet_manager.cpp` (SQLite schema v3 + persistence)
- `include/wallet/wallet_manager.h`
- `src/rpc/rpc_meta.cpp`
- `tests/rpc/test_rpc_capabilities.cpp`
- `tests/e2e/test_wallet_mining_rpc.py`, `tests/e2e/test_wrong_network_hrp.py`
- `scripts/ci/run_e2e_tests.sh`
- **Removed**: RPC v2 (all hooks and build flags)

## Security notes
- Mining address must be wallet-owned (enforced).
- Address HRP must match active network (enforced).
- Cookie auth enforced on all endpoints.

## CI
- Adds e2e run script: `scripts/ci/run_e2e_tests.sh`
- Workflow runs daemon on regtest, executes 8/8 tests.
