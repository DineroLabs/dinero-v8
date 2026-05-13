# RELEASE CHECKLIST (DineroCoin)

**Scope:** genesis/meta bootstrap & minimal RPC suitable for GUI/CLI bring-up.

### A. Preflight

* [ ] `test_db_init_idempotence` → **PASS**
* [ ] `scripts/db_audit.sh regtest` → no mismatches
* [ ] `getblockhash 0` returns the expected genesis (lowercase) via RPC
* [ ] `EnsureGenesisMeta` called before RPC/P2P start (verified by logs)

### B. Chainparams & Networks

* [ ] `chainparams` exposes correct `NetworkIDString()` and `GenesisHashHex()`
* [ ] Placeholders for testnet/mainnet **not** used for real datadirs
* [ ] (When ready) mined testnet/mainnet genesis committed & version bumped

### C. Build & CMake

* [ ] `db_init_simple.cpp` & `db_meta_utils.cpp` included in `dinerod`
* [ ] No stray `${!network}` or bash indirection in scripts (`grep -R '\${![^}]\+}' scripts -n` is empty)

### D. Scripts & Tooling

* [ ] `fix_genesis_meta.sh` present, idempotent, RPC fallback works
* [ ] `db_audit.sh` passes on fresh and reused datadirs
* [ ] `smoke_check.sh` runs green locally
* [ ] `smoke_test_final.sh` comprehensive end-to-end test passes

### E. CI

* [ ] CI job runs `fix_genesis_meta.sh`, `db_audit.sh`, and idempotence test
* [ ] CI fails build on any audit mismatch
* [ ] `scripts/ci_db_check.sh` integrated into workflow

### F. RPC Surface (MVP)

* [ ] `getblockhash` registered; height 0 via meta
* [ ] (Optional) `getblockchaininfo` returns chain, blocks, bestblockhash, chainwork
* [ ] All RPC methods return lowercase hex (Bitcoin standard)

### G. GUI/CLI UX

* [ ] Header shows `Network • Height • BestHash(8) • Genesis(8)`
* [ ] On network switch: silent audit + visible error if mismatch
* [ ] Case-insensitive hash comparisons throughout

### H. Packaging & Docs

* [ ] `DAEMON_INTEGRATION.md` included in repo
* [ ] Release notes mention canonical hex policy (lowercase)
* [ ] Smoke command snippet included in release notes
* [ ] Migration guide for existing datadirs

### I. Rollback Plan

* [ ] If regression: disable fast-fail guard behind a temporary flag **only** for recovery, fix root cause, re-enable guard

---

**Release gate:** Ship only if A–H are all ✅.

## Quick Validation Commands

```bash
# Pre-release smoke test
./scripts/fix_genesis_meta.sh regtest --datadir ./data
./build/test_db_init_idempotence
./scripts/db_audit.sh regtest
./smoke_test_final.sh

# Check for bash indirection
grep -R '\${![^}]\+}' scripts -n || echo "✅ Clean"

# Verify RPC works
./build/dinerod -regtest -datadir=./data -daemon
sleep 5
curl -s --user "$(cat ./data/regtest/.cookie)" \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":"test","method":"getblockhash","params":[0]}' \
  http://127.0.0.1:20998/ | jq -r .result
```

## Success Criteria

✅ **Database initialization is idempotent and crash-safe**  
✅ **Network validation prevents datadir confusion**  
✅ **RPC methods return consistent lowercase hashes**  
✅ **All scripts are portable and bulletproof**  
✅ **CI gates prevent regressions**  

**🚀 Ready to ship the bulletproof database system!**
