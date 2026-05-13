# v0.4.0 — Wallet-Aware Mining RPC (production-ready)

## Summary
This release lands a canonical, wallet-scoped RPC stack with secure mining address management and full SQLite persistence.

## ✨ What's new
- **Wallet-scoped URLs**: call RPCs under `/wallet/<name>` to eliminate context ambiguity.
- **Mining RPCs**: `mining.getaddress`, `mining.setaddress` with strict wallet-ownership + network HRP validation.
- **Wallet RPCs**: `wallet.create`, `wallet.load`, `wallet.getnewaddress`, `wallet.validateaddress`, `wallet.listaddresses`.
- **Meta RPCs**: `rpc.capabilities`, `rpc.listmethods`, `rpc.help`, `rpc.health` (for probes/readiness).
- **Persistence**: SQLite schema v3 with network-scoped storage for mining address (survives restarts).
- **Dispatcher**: single JSON-RPC dispatcher; legacy v2 removed to prevent handler conflicts.
- **Tests**: comprehensive end-to-end wallet-aware mining tests (8/8 green).

## 🔒 Security
- Mining address must be owned by the active or scoped wallet; otherwise it's rejected.
- Address HRP must match the active network (e.g., `rdin` on regtest).

## 🧪 Quick verify
```bash
# Start daemon (regtest)
DATADIR="$HOME/Library/Application Support/DineroCoin/Dinero All-in-One/data/regtest"
./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=18443 --wsport=18444

# Auth
AUTH="$(tr -d '\r\n' < "$DATADIR/.cookie")"

# Health & capabilities
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"rpc.health"}' \
  http://127.0.0.1:18443/

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":2,"method":"rpc.capabilities"}' \
  http://127.0.0.1:18443/

# Wallet lifecycle (scoped URL)
RPC="http://127.0.0.1:18443/wallet/test_wallet"
curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":3,"method":"wallet.create","params":{"name":"test_wallet"}}' \
  http://127.0.0.1:18443/

ADDR=$(curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":4,"method":"wallet.getnewaddress"}' "$RPC" | jq -r .result.address)

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"]}" \
  http://127.0.0.1:18443/

curl -s -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":6,"method":"mining.getaddress"}' \
  http://127.0.0.1:18443/
```

## ⚠️ Breaking changes
Legacy bare RPCs (e.g., `getnewaddress`) are superseded by namespaced ones (e.g., `wallet.getnewaddress`).
If you need compatibility, use the canonicalizer's aliasing.

## 📄 Notable files
- `src/daemon/rpc/WalletHandlers.{h,cpp}`
- `src/daemon/rpc/MiningHandlers.{h,cpp}`
- `src/daemon/rpc_server.cpp` (registration + URL scoping)
- `src/rpc/daemon_rpc_dispatch.cpp`
- `include/rpc/{rpc_canonicalizer.h,rpc_aliases.h}`
- `Migrations: migrations/002_wallets.sql, migrations/003_wallet_settings.sql`
- `Tests: tests/rpc/test_rpc_capabilities.cpp, tests/e2e/test_mining_rpc_comprehensive.py`
- `Docs: docs/rpc/{meta.md,wallet.md,mining.md}`
- **Removed**: RPC v2 implementation and registrations
