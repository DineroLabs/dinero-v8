# DAEMON_INTEGRATION.md (DineroCoin)

> One-pager to wire **idempotent database initialization** and **genesis validation** into `dinerod`, with smoke tests and CI gates.

## 0) Goal

Ensure every daemon start has a valid `meta` table (`network`, `genesis_hash`, `besthash`, `height`, `chainwork`), fails fast on mismatches, and exposes minimal RPC needed by GUI/CLI.

## 1) Prereqs

* SQLite opened for `blockchain.db` (journal mode WAL recommended)
* Chain params expose:
  * `NetworkIDString()` → `"regtest"|"testnet"|"mainnet"`
  * `GenesisHashHex()` → 64-char hex (lowercase preferred)
* Helpers available:
  * `EnsureGenesisMeta(sqlite3* db, std::string net, std::string genesis_hex)`
  * `ReadMeta(sqlite3* db, const char* key)`

## 2) Integration (copy/paste)

**Where:** after opening `blockchain.db` and **before** starting RPC/P2P.

```cpp
#include "daemon/db_init_simple.hpp"  // EnsureGenesisMeta
#include "daemon/db_meta_utils.hpp"   // ReadMeta
#include "chainparams.h"

sqlite3* dbh = OpenBlockchainDB(...); // your existing open

const auto& params = Params();
const std::string net    = params.NetworkIDString();      // "regtest"|"testnet"|"mainnet"
const std::string genesis= params.GenesisHashHex();       // 64-char hex (lowercase policy)

const bool changed = dinero::db::EnsureGenesisMeta(dbh, net, genesis);
if (changed) LOG_INFO() << "Meta initialized/updated for " << net;

// Fail fast if the datadir belongs to another network or bad params
auto toLower = [](std::string s){ for (auto& c : s) c = std::tolower(c); return s; };
const std::string meta_genesis = dinero::db::ReadMeta(dbh, "genesis_hash");
if (meta_genesis.empty()) {
  LOG_FATAL() << "Missing genesis_hash in meta after initialization";
  std::terminate();
}
if (toLower(meta_genesis) != toLower(genesis)) {
  LOG_FATAL() << "Genesis mismatch: meta=" << meta_genesis
              << " vs chainparams=" << genesis
              << " (wrong datadir/network)";
  std::terminate();
}
```

### Update meta on tip changes

Persist best hash/height/chainwork atomically when the tip advances:

```sql
INSERT INTO meta(key,value) VALUES('besthash', :hash)
  ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta(key,value) VALUES('height', CAST(:height AS TEXT))
  ON CONFLICT(key) DO UPDATE SET value=excluded.value;
INSERT INTO meta(key,value) VALUES('chainwork', :chainwork)
  ON CONFLICT(key) DO UPDATE SET value=excluded.value;
```

## 3) Minimal RPC (unblock GUI/CLI)

* **Policy:** return **lowercase** hex in RPC; treat comparisons case-insensitively in code.

```cpp
Json::Value RpcGetBlockHash(const Json::Value& params) {
  if (!params.isArray() || params.size()!=1 || !params[0].isIntegral())
    throw RpcError{-32602, "getblockhash height"};
  const int64_t h = params[0].asInt64();
  if (h < 0) throw RpcError{-8, "Block height out of range"};

  if (h == 0) {
    std::string g = dinero::db::ReadMeta(dbh, "genesis_hash");
    std::transform(g.begin(), g.end(), g.begin(), ::tolower);
    if (g.empty()) throw RpcError{-1, "Genesis not set"};
    return g;
  }
  // TODO: lookup from blocks index
  auto hh = db::QueryBlockHashByHeight(dbh, h);
  if (hh.empty()) throw RpcError{-8, "Block height out of range"};
  std::transform(hh.begin(), hh.end(), hh.begin(), ::tolower);
  return hh;
}
// registry.add("getblockhash", &RpcGetBlockHash);
```

**Optional:** minimum viable `getblockchaininfo` using meta values.

## 4) CMake wiring

```cmake
# daemon/CMakeLists.txt
target_sources(dinerod PRIVATE
  src/daemon/db_init_simple.cpp
  src/daemon/db_meta_utils.cpp
)
target_include_directories(dinerod PRIVATE ${CMAKE_SOURCE_DIR}/src)
```

## 5) Smoke test (local)

```bash
# Ensure meta (no-op if already good)
./scripts/fix_genesis_meta.sh regtest --datadir ./data

# Start daemon
./build/dinerod -regtest -datadir=./data -daemon

# DB audit
./scripts/db_audit.sh regtest

# RPC check (height 0)
curl -s --user "$(cat ./data/regtest/.cookie)" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":"t","method":"getblockhash","params":[0]}' \
  http://127.0.0.1:20998/ | jq -r .result
```

## 6) CI gate (snippet)

```yaml
- name: DB bootstrap + audit (regtest)
  run: |
    ./scripts/fix_genesis_meta.sh regtest --datadir ./data
    ./scripts/db_audit.sh regtest
    ./build/test_db_init_idempotence
```

## 7) Security & invariants

* Datadir **must** match selected network (fail fast on mismatch)
* RPC/Logs use **lowercase** hex; DB reads/writes are normalized on read
* All meta mutations in a single transaction (no partial writes)