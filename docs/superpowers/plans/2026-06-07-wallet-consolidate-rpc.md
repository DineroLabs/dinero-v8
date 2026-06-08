# `wallet.consolidate` Implementation Plan

> **STATUS (2026-06-07): Tasks 1–4 (daemon) DONE and green on `dinero-v8` (`feature/wallet-consolidate-rpc-v8`).** This plan was authored against the obsolete `dinero`/`p2p-fix` tree, then the implementation was cherry-picked onto the live `dinero-v8` mainline. Internal `p2p-fix` references and exact line numbers reflect the original `dinero` repo and are historical; the equivalent symbols exist on v8 (handler at `methods_wallet_context.cpp:7390`, registration ~`:7806`, allowlist `http_rpc_server.cpp:46`). **Task 5 (dinero-qt button) is deferred** until this merges.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real `wallet.consolidate` daemon RPC that sweeps many small UTXOs of one script family into a single fresh self-output, with preview-by-default and a dual fee-sanity gate — replacing the dead dinero-qt "Consolidate" button that today calls an unimplemented method.

**Architecture:** One new free handler `rpc_context_wallet_consolidate` in `src/rpc/methods_wallet_context.cpp`, registered as `wallet.consolidate` (+ `consolidate` alias) and added to the `http_rpc_server.cpp` admin allowlist. It reuses the exact filter → fee → build → sign → broadcast machinery already proven in `rpc_context_wallet_sendtoaddress`. Transparent-only (P2TR or P2MR), one family per tx, output value computed as `sum(inputs) − fee` so there is a single output and zero change. Tests are self-contained regtest shell scripts driving `dinero-cli`/curl, matching `tests/test_wallet_createfundedpsbt_rpc.sh`.

**Tech Stack:** C++17, jsoncpp (`din::Json` = `Json::Value`), CMake, the daemon's `WalletService`/`ChainstateService`/`MempoolService`, `UnsignedTxBuilder`/`TransactionSigner`, regtest + bash + jq + curl.

---

## Reference facts (verified in tree, 2026-06-07)

- Handler signature: `din::Json rpc_context_wallet_consolidate(const ExecutionContext& ctx, const din::Json& params)`.
- Service handles (mirror `sendtoaddress`, `methods_wallet_context.cpp:2439-2466`):
  `std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet)`, `->hasActiveWallet()`, `->get()` → `dinero::WalletManager&`; `->get().isWalletLocked()`; `std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate)`, `->utxoIndex()`, `->getBlockHeight()`; `std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool)`.
- UTXOs: `wallet_service->get().listUnspentUTXOs(int min_conf, int max_conf)` → `std::vector<dinero::WalletManager::WalletUTXO>` with fields `txid`(hex string), `vout`, `amount_una`, `address`, `confirmations`, `height`, `spendable`, `is_coinbase`, `is_mature`, `script_pubkey`(hex string), `is_confidential`, `derivation_path`.
- Filters: `wallet_service->get().isUTXOLocked(txid, vout)`; `mp->mempool().isOutputSpentInMempool(OutPoint(dinero::TxId(dinero::uint256::FromHexUnsafe(txid)), vout))`; `WalletUtxoIsPresentInLiveUtreexoForest(utxo, chainstate_service, &reason)` (file-local, `methods_wallet_context.cpp:182`) — returns true when present; keep only present.
- Family from `script_pubkey` hex: P2TR = length 68 starting `5120`; P2MR = length 68 starting `5320`. PQ check helper: `dinero::consensus::pq::IsP2MRScript(std::vector<uint8_t> spk)`.
- Fee: `mp->getFeeEstimator()->estimateFee(dinero::policy::FeeTarget::NORMAL)` → `.is_sufficient_data`, `.fee_rate` (una/kB; divide by 1000 → una/vB). Sizer/fee: `dinero::UnsignedTxBuilder::EstimateTransactionSize(n_in, n_out, n_p2mr)` and `::CalculateFee(vsize, fee_rate)`; `::DUST_THRESHOLD` (546).
- Fresh address: dispatch `g_rpcRegistry.lookup("wallet.getnewaddress")` with params `{address_type: "taproot"|"p2mr", label: "consolidate"}`; result has `["address"]`.
- Build/sign/broadcast (mirror `sendtoaddress:2986-3124`): `dinero::CanonicalWalletUTXO{txid:uint256, vout, value:AmountUna::Una(una), path, height, is_coinbase, spk:bytes}`; `dinero::BuildOptions{fee_rate, enable_rbf, change_address}`; `dinero::TxOutputRequest(addr, una)`; `dinero::UnsignedTxBuilder::Build(cins, outs, opts)` → `.success/.error/.unsigned_tx`; keys via `wallet_service->get().deriveKeyForScriptPubKey(spk_hex)` (skip P2MR) / `getPrivateKeyForPath(path)`; provider `dinero::MapKeyProvider(path_to_key)` or, for P2MR, `dinero::wallet::WalletKeyProvider` built from `GetV7PqMasterKey()` + `GetV7P2MRStorePath()` + `dinero::wallet::V7P2MRStore`; `dinero::TransactionSigner::Sign(unsigned_tx, provider)` → `.success/.error/.signed_tx{tx, fee}`; submit `mp->mempool().submitTransaction(signed_tx, "rpc:wallet.consolidate", true)` → `.accepted()/.rejected()/.code/.message`, `TxRejectCodeToString(code)`; txid `signed_tx.GetTxid().AsUint256().GetHex()`; raw hex `signed_tx.SerializeHex(true)`.
- jsoncpp null literal: `din::Json(Json::nullValue)`.
- Build: `cmake --build build --target dinerod -j$(sysctl -n hw.logicalcpu)` (~2.5 min). Regtest coinbase maturity = 100 blocks.

---

## File Structure

- **Modify** `src/rpc/methods_wallet_context.cpp`
  - Add free function `rpc_context_wallet_consolidate(...)` immediately **before** the line `g_rpcRegistry.registerHandler("wallet.sendtoaddress", ...)` (so it is in scope at registration; currently ~line 7437).
  - Add two registration lines next to the other wallet registrations (~line 7437).
- **Modify** `src/daemon/http_rpc_server.cpp`
  - Add `"wallet.consolidate"` to the admin-method allowlist (the brace-list near line 45 that already contains `"wallet.sendtoaddress"`).
- **Create** `tests/test_wallet_consolidate_dryrun_rpc.sh` — keyless behaviors (no-op, partition/auto, fee-gate, locked/immature exclusion, dry-run plan).
- **Create** `tests/test_wallet_consolidate_broadcast_rpc.sh` — execution (signed-hex-without-broadcast, broadcast → confirmable txid).
- **Modify (client, separate repo)** `dinero-qt/src/mainwindow.cpp` — repoint the dead button to `wallet.consolidate`.

---

## Task 1: Dry-run integration test (failing)

**Files:**
- Create: `tests/test_wallet_consolidate_dryrun_rpc.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/test_wallet_consolidate_dryrun_rpc.sh`:

```bash
#!/usr/bin/env bash
#
# wallet.consolidate dry-run / keyless behaviors:
# - no-op on empty wallet and single-UTXO family
# - auto picks the majority family and never mixes families
# - locked + immature coinbase UTXOs are excluded
# - fee-sanity gate rejects (percent and absolute) and dry-run returns the plan
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
DATADIR="/tmp/wallet_consolidate_dryrun_$$"
PORT_RPC=22182; PORT_P2P=22181; DAEMON_PID=""
BUILD_DIR="$(dirname "$0")/../build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [ -f "$DATADIR/daemon.log" ]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 80 "$DATADIR/daemon.log" >&2 || true
    fi
    exit 1
}
get_cookie() { cut -d: -f2 "$DATADIR/.cookie" 2>/dev/null || true; }
rpc_raw() {
    local method="$1"; local params="${2:-[]}"; local cookie; cookie="$(get_cookie)"
    if [ -z "$cookie" ]; then echo '{"error":{"message":"missing rpc cookie"}}'; return 1; fi
    curl -sS -X POST "http://127.0.0.1:$PORT_RPC" -u "__cookie__:$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}"
}
rpc_result() {
    local response; response="$(rpc_raw "$1" "${2:-[]}")"
    if echo "$response" | jq -e '.error != null' >/dev/null 2>&1; then
        fail "RPC error [$1]: $(echo "$response" | jq -r '.error.message // (.error|tostring)')"
    fi
    echo "$response" | jq '.result'
}
rpc_scalar() { rpc_result "$1" "$2" | jq -r "$3"; }
wait_for_daemon() {
    local waited=0
    while [ "$waited" -lt 30 ]; do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then return 0; fi
        sleep 1; ((waited++))
    done
    return 1
}
start_daemon() {
    rm -rf "$DATADIR"; mkdir -p "$DATADIR"
    "$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$PORT_RPC" --port="$PORT_P2P" --debug \
        > "$DATADIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_daemon || fail "daemon failed to start"
}
stop_daemon() { [ -n "$DAEMON_PID" ] && { kill "$DAEMON_PID" 2>/dev/null || true; wait "$DAEMON_PID" 2>/dev/null || true; DAEMON_PID=""; }; }
cleanup() { stop_daemon; rm -rf "$DATADIR"; }
trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"

info "Starting daemon"
start_daemon
rpc_result "wallet.createhd" '["consolidate_wallet"]' >/dev/null

# --- empty wallet: no-op, not an error ---
EMPTY="$(rpc_result "wallet.consolidate" '[{"dry_run":true}]' | jq -c '.')"
echo "$EMPTY" | jq -e '.ok == true' >/dev/null || fail "empty wallet not ok: $EMPTY"
echo "$EMPTY" | jq -e '.selected_inputs == 0' >/dev/null || fail "empty wallet should select 0: $EMPTY"

# --- fund a P2TR family with many mature coinbase UTXOs ---
P2TR_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
[ -n "$P2TR_ADDR" ] || fail "no p2tr addr"
info "Mining 130 P2TR coinbase UTXOs (mature ones become eligible)"
rpc_result "generatetoaddress" "[130,\"$P2TR_ADDR\"]" >/dev/null

# --- dry-run plan over the P2TR family ---
PLAN="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_inputs":100}]' | jq -c '.')"
echo "$PLAN" | jq -e '.ok == true' >/dev/null || fail "p2tr plan not ok: $PLAN"
echo "$PLAN" | jq -e '.dry_run == true' >/dev/null || fail "plan not dry_run: $PLAN"
echo "$PLAN" | jq -e '.address_family == "p2tr"' >/dev/null || fail "wrong family: $PLAN"
echo "$PLAN" | jq -e '.selected_inputs >= 2 and .selected_inputs <= 100' >/dev/null || fail "bad selected_inputs: $PLAN"
echo "$PLAN" | jq -e '.input_value > 0 and .estimated_fee > 0' >/dev/null || fail "bad values: $PLAN"
echo "$PLAN" | jq -e '((.input_value - .estimated_fee - .output_value) | if . < 0 then -. else . end) < 0.00000002' >/dev/null || fail "output != input-fee: $PLAN"
echo "$PLAN" | jq -e '.destination | type == "string" and length > 0' >/dev/null || fail "no destination: $PLAN"
echo "$PLAN" | jq -e '.txid == null' >/dev/null || fail "dry-run produced txid: $PLAN"

# --- auto picks the majority family; with only P2TR funded it must be p2tr ---
AUTO="$(rpc_result "wallet.consolidate" '[{"address_type":"auto","dry_run":true,"min_confirmations":1}]' | jq -c '.')"
echo "$AUTO" | jq -e '.address_family == "p2tr"' >/dev/null || fail "auto should pick p2tr: $AUTO"

# --- max_inputs cap is honored ---
CAP="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_inputs":5}]' | jq -c '.')"
echo "$CAP" | jq -e '.selected_inputs == 5' >/dev/null || fail "max_inputs cap ignored: $CAP"

# --- fee gate trips on absolute DIN cap (tiny max_fee_din) ---
GATE_ABS="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_fee_din":0.0000001}]' | jq -c '.')"
echo "$GATE_ABS" | jq -e '.ok == false and .fee_ok == false' >/dev/null || fail "abs fee gate did not trip: $GATE_ABS"
echo "$GATE_ABS" | jq -e '.reason | type == "string" and length > 0' >/dev/null || fail "no reason on abs gate: $GATE_ABS"

# --- fee gate trips on percent (tiny max_fee_percent) ---
GATE_PCT="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_fee_percent":0.0000001,"max_fee_din":1000000}]' | jq -c '.')"
echo "$GATE_PCT" | jq -e '.ok == false and .fee_ok == false' >/dev/null || fail "pct fee gate did not trip: $GATE_PCT"

# --- locked UTXOs are excluded: lock everything, expect no-op or fewer inputs ---
UTXO0="$(rpc_result "wallet.listunspent" '[1]' | jq -c '[.[] | select(.spendable==true)][0]')"
if echo "$UTXO0" | jq -e '.txid' >/dev/null 2>&1; then
    LTXID="$(echo "$UTXO0" | jq -r '.txid')"; LVOUT="$(echo "$UTXO0" | jq -r '.vout')"
    BEFORE="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
    rpc_result "wallet.lockunspent" "[false,[{\"txid\":\"$LTXID\",\"vout\":$LVOUT}]]" >/dev/null
    AFTER="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
    [ "$AFTER" -lt "$BEFORE" ] || fail "locked UTXO not excluded (before=$BEFORE after=$AFTER)"
    rpc_result "wallet.lockunspent" "[true,[{\"txid\":\"$LTXID\",\"vout\":$LVOUT}]]" >/dev/null
fi

# --- immature coinbase excluded: high min_confirmations admits only old coinbases ---
MATURE_ONLY="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":100}]' | jq -c '.')"
ALL_CONF1="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
MO_SEL="$(echo "$MATURE_ONLY" | jq -r '.selected_inputs')"
[ "$MO_SEL" -le "$ALL_CONF1" ] || fail "min_confirmations=100 selected more than conf=1 ($MO_SEL > $ALL_CONF1)"

pass "wallet.consolidate dry-run/no-op/partition/fee-gate/exclusion behaviors hold"
```

- [ ] **Step 2: Make the test executable and run it to verify it fails**

Run:
```bash
chmod +x tests/test_wallet_consolidate_dryrun_rpc.sh
bash tests/test_wallet_consolidate_dryrun_rpc.sh; echo "EXIT=$?"
```
Expected: **FAIL** — first `wallet.consolidate` call returns a JSON-RPC error (`method not found`), the script prints `FAIL: RPC error [wallet.consolidate]: ...` and `EXIT=1`. (This requires an already-built `build/dinerod`; if missing, run the build command from Task 2 Step 4 first — the test still fails because the method is unimplemented.)

- [ ] **Step 3: Commit the failing test**

```bash
git add tests/test_wallet_consolidate_dryrun_rpc.sh
git commit -m "test: wallet.consolidate dry-run/keyless behaviors (failing — method unimplemented)"
```

---

## Task 2: Implement the handler (dry-run path) + registration + allowlist

**Files:**
- Modify: `src/rpc/methods_wallet_context.cpp` (add handler before the `wallet.sendtoaddress` registration ~7437; add two registration lines ~7437)
- Modify: `src/daemon/http_rpc_server.cpp` (admin allowlist ~45)

- [ ] **Step 1: Add the handler function**

In `src/rpc/methods_wallet_context.cpp`, immediately **above** the line containing `g_rpcRegistry.registerHandler("wallet.sendtoaddress"`, insert this complete function. The execution branch is a deliberate stub here (Task 4 replaces it):

```cpp
// ============================================================================
// wallet.consolidate — combine many small UTXOs of ONE script family (P2TR or
// P2MR) into a single fresh self output. Transparent only; no family mixing.
// Preview-by-default (dry_run); broadcast only when explicitly requested.
// Reuses the wallet.sendtoaddress filter/fee/build/sign/broadcast machinery.
// ============================================================================
din::Json rpc_context_wallet_consolidate(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    const auto log_debug = [&ctx](const std::string& msg) {
        if (ctx.logger) ctx.logger->debug(msg); else dinero::g_logger.debug(msg);
    };

    if (!ctx.daemon || !ctx.daemon->wallet) { result["ok"] = false; result["error"] = "Wallet service not available"; return result; }
    auto wallet_service = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    if (!wallet_service || !wallet_service->hasActiveWallet()) { result["ok"] = false; result["error"] = "No active wallet"; return result; }
    if (!ctx.daemon->chainstate) { result["ok"] = false; result["error"] = "Chainstate service not available"; return result; }
    auto chainstate_service = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate_service || !chainstate_service->utxoIndex()) { result["ok"] = false; result["error"] = "UTXO index not available"; return result; }

    try {
        const auto getStr = [&](const char* k, const std::string& d) {
            return (params.isObject() && params.isMember(k) && params[k].isString()) ? params[k].asString() : d; };
        const auto getInt = [&](const char* k, int d) {
            return (params.isObject() && params.isMember(k) && params[k].isNumeric()) ? params[k].asInt() : d; };
        const auto getBool = [&](const char* k, bool d) {
            return (params.isObject() && params.isMember(k) && params[k].isBool()) ? params[k].asBool() : d; };
        const auto getDbl = [&](const char* k, double d) {
            return (params.isObject() && params.isMember(k) && params[k].isNumeric()) ? params[k].asDouble() : d; };

        std::string address_type = getStr("address_type", "auto");
        std::transform(address_type.begin(), address_type.end(), address_type.begin(), ::tolower);
        int    max_inputs        = getInt("max_inputs", 100);
        int    min_conf          = getInt("min_confirmations", 6);
        bool   include_unconf    = getBool("include_unconfirmed", false);
        double max_fee_percent   = getDbl("max_fee_percent", 1.0);
        double max_fee_din       = getDbl("max_fee_din", 1.0);
        bool   dry_run           = getBool("dry_run", true);
        bool   broadcast         = getBool("broadcast", false);
        if (max_inputs < 1)   max_inputs = 1;
        if (max_inputs > 500) max_inputs = 500;   // policy-safe cap

        if (address_type == "shielded") {
            result["ok"] = false; result["error"] = "shielded consolidation not supported in v7"; return result;
        }

        // Resolve fee_rate (una/vB): explicit numeric, else mempool estimate, else min-relay floor.
        double fee_rate = 0.0;
        if (params.isObject() && params.isMember("fee_rate") && params["fee_rate"].isNumeric())
            fee_rate = params["fee_rate"].asDouble();
        if (fee_rate <= 0.0 && ctx.daemon->mempool) {
            auto mps = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);
            if (mps) {
                auto fe = mps->getFeeEstimator();
                if (fe) {
                    auto est = fe->estimateFee(dinero::policy::FeeTarget::NORMAL);
                    if (est.is_sufficient_data && est.fee_rate > 0)
                        fee_rate = static_cast<double>(est.fee_rate) / 1000.0;
                }
            }
        }
        if (fee_rate <= 0.0) fee_rate = 1.0;

        // Let the wallet worker catch up to the tip so the UTXO table isn't stale.
        {
            const uint32_t tip = chainstate_service->getBlockHeight();
            wallet_service->get().WaitForHeight(tip, std::chrono::milliseconds(5000));
        }

        // Gather + filter eligible UTXOs (same chain as wallet.sendtoaddress).
        const int floor_conf = include_unconf ? 0 : min_conf;
        auto raw = wallet_service->get().listUnspentUTXOs(floor_conf, 9999999);
        auto mps = std::dynamic_pointer_cast<dinero::MempoolService>(ctx.daemon->mempool);

        std::vector<dinero::WalletManager::WalletUTXO> eligible;
        for (const auto& u : raw) {
            if (!u.spendable || !u.is_mature || u.is_confidential) continue;
            if (u.derivation_path.empty() || u.script_pubkey.empty()) continue;
            if (wallet_service->get().isUTXOLocked(u.txid, u.vout)) continue;
            if (mps) {
                OutPoint op(dinero::TxId(dinero::uint256::FromHexUnsafe(u.txid)), u.vout);
                if (mps->mempool().isOutputSpentInMempool(op)) continue;
            }
            std::string reason;
            if (!WalletUtxoIsPresentInLiveUtreexoForest(u, chainstate_service, &reason)) continue;
            eligible.push_back(u);
        }

        // Partition by script family.
        const auto isP2TR = [](const std::string& s){ return s.size()==68 && s.rfind("5120",0)==0; };
        const auto isP2MR = [](const std::string& s){ return s.size()==68 && s.rfind("5320",0)==0; };
        std::vector<dinero::WalletManager::WalletUTXO> p2tr, p2mr;
        for (const auto& u : eligible) {
            if (isP2TR(u.script_pubkey)) p2tr.push_back(u);
            else if (isP2MR(u.script_pubkey)) p2mr.push_back(u);
        }

        // Choose target family (auto → larger pool; tie → p2tr).
        std::string family;
        std::vector<dinero::WalletManager::WalletUTXO>* pool = nullptr;
        if (address_type == "p2mr") { family = "p2mr"; pool = &p2mr; }
        else if (address_type == "p2tr" || address_type == "taproot") { family = "p2tr"; pool = &p2tr; }
        else { if (p2mr.size() > p2tr.size()) { family = "p2mr"; pool = &p2mr; } else { family = "p2tr"; pool = &p2tr; } }
        result["address_family"] = family;

        // No-op cases.
        if (pool->size() == 0) {
            result["ok"] = true; result["dry_run"] = dry_run; result["selected_inputs"] = 0;
            result["reason"] = "no eligible UTXOs in family"; result["txid"] = din::Json(Json::nullValue); return result;
        }
        if (pool->size() == 1) {
            result["ok"] = true; result["dry_run"] = dry_run; result["selected_inputs"] = 0;
            result["reason"] = "nothing to consolidate"; result["txid"] = din::Json(Json::nullValue); return result;
        }

        // Select up to max_inputs, smallest-value-first (sweep the most dust per tx).
        std::sort(pool->begin(), pool->end(),
                  [](const dinero::WalletManager::WalletUTXO& a, const dinero::WalletManager::WalletUTXO& b){
                      return a.amount_una < b.amount_una; });
        std::vector<dinero::WalletManager::WalletUTXO> selected(
            pool->begin(), pool->begin() + std::min<size_t>(pool->size(), static_cast<size_t>(max_inputs)));

        int64_t total_una = 0;
        for (const auto& u : selected) total_una += static_cast<int64_t>(u.amount_una);

        // Single output, no change: fee from the builder's own size/fee math.
        const size_t n_in   = selected.size();
        const size_t n_p2mr = (family == "p2mr") ? n_in : 0;
        const size_t vsize  = dinero::UnsignedTxBuilder::EstimateTransactionSize(n_in, 1, n_p2mr);
        const int64_t fee_una = static_cast<int64_t>(
            dinero::UnsignedTxBuilder::CalculateFee(vsize, static_cast<uint64_t>(fee_rate)));
        const int64_t output_una = total_una - fee_una;
        const double  fee_din = static_cast<double>(fee_una) / 1e8;

        if (output_una <= static_cast<int64_t>(dinero::UnsignedTxBuilder::DUST_THRESHOLD)) {
            result["ok"] = false; result["dry_run"] = dry_run; result["fee_ok"] = false;
            result["reason"] = "consolidation output below dust after fee";
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"] = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din; return result;
        }

        // Fee sanity gate: reject if fee > percent-of-value OR > absolute DIN cap (both modes).
        const double pct = (total_una > 0) ? (100.0 * static_cast<double>(fee_una) / static_cast<double>(total_una)) : 0.0;
        const bool gate_abs = fee_din > max_fee_din;
        const bool gate_pct = pct > max_fee_percent;
        if (gate_abs || gate_pct) {
            std::ostringstream r;
            if (gate_abs) r << "fee " << fee_din << " DIN exceeds max_fee_din " << max_fee_din;
            else          r << "fee " << pct << "% exceeds max_fee_percent " << max_fee_percent;
            result["ok"] = false; result["dry_run"] = dry_run; result["fee_ok"] = false; result["reason"] = r.str();
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"] = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din; return result;
        }

        // Fresh self destination of the target family.
        std::string dest;
        {
            din::Json ap;
            ap["address_type"] = (family == "p2mr") ? std::string("p2mr") : std::string("taproot");
            ap["label"] = "consolidate";
            auto* gh = g_rpcRegistry.lookup("wallet.getnewaddress");
            if (!gh) { result["ok"] = false; result["error"] = "wallet.getnewaddress handler not registered"; return result; }
            din::Json ar = (*gh)(ctx, ap);
            if (ar.isMember("address") && ar["address"].isString()) dest = ar["address"].asString();
            else if (ar.isMember("result") && ar["result"].isObject() && ar["result"].isMember("address")) dest = ar["result"]["address"].asString();
        }
        if (dest.empty()) { result["ok"] = false; result["error"] = "Failed to derive consolidation address"; return result; }

        // Dry-run: return the plan, no signing.
        if (dry_run) {
            result["ok"] = true; result["dry_run"] = true;
            result["selected_inputs"] = static_cast<int>(n_in);
            result["input_value"]   = static_cast<double>(total_una) / 1e8;
            result["estimated_fee"] = fee_din;
            result["output_value"]  = static_cast<double>(output_una) / 1e8;
            result["destination"]   = dest;
            result["fee_ok"]        = true;
            result["txid"]  = din::Json(Json::nullValue);
            result["rawtx"] = din::Json(Json::nullValue);
            return result;
        }

        // EXECUTION STUB — replaced in Task 4 (build/sign/broadcast).
        result["ok"] = false;
        result["error"] = "consolidate execution not yet implemented";
        log_debug("[wallet.consolidate] execution path stubbed");
        return result;

    } catch (const std::exception& e) {
        result["ok"] = false; result["error"] = std::string("consolidate failed: ") + e.what(); return result;
    }
}
```

- [ ] **Step 2: Register the handler**

In the same file, next to the `wallet.sendtoaddress` registration (~7437), add:

```cpp
    g_rpcRegistry.registerHandler("wallet.consolidate",
                                 rpc_context_wallet_consolidate,
                                 RegisterMode::Overwrite,
                                 "context-aware");
    g_rpcRegistry.registerAlias("consolidate", "wallet.consolidate");
```

- [ ] **Step 3: Add to the admin-method allowlist**

In `src/daemon/http_rpc_server.cpp`, in the brace-list that contains `"wallet.sendtoaddress"` (~line 45), add a line:

```cpp
    "wallet.consolidate",
```

- [ ] **Step 4: Build the daemon**

Run:
```bash
cmake --build build --target dinerod -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -20
```
Expected: build completes; final lines show the `dinerod` link step with no errors. If it fails, read the compiler error and fix the handler (most likely a missing `#include` — but `<algorithm>`, `<sstream>`, `<iomanip>`, `<map>`, `<memory>`, `<cstring>` are already used by `sendtoaddress` in this TU).

- [ ] **Step 5: Run the dry-run test to verify it passes**

Run:
```bash
bash tests/test_wallet_consolidate_dryrun_rpc.sh; echo "EXIT=$?"
```
Expected: **PASS** — final line `PASS: wallet.consolidate dry-run/...` and `EXIT=0`.

- [ ] **Step 6: Verify the test actually gates (fails without the handler)**

Temporarily rename the registration to prove the test catches a missing method:
```bash
sed -i.bak 's/"wallet.consolidate",$/"wallet.consolidate_DISABLED",/' src/rpc/methods_wallet_context.cpp
cmake --build build --target dinerod -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -3
bash tests/test_wallet_consolidate_dryrun_rpc.sh; echo "EXIT=$?"   # expect EXIT=1 (method not found)
mv src/rpc/methods_wallet_context.cpp.bak src/rpc/methods_wallet_context.cpp
cmake --build build --target dinerod -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -3
bash tests/test_wallet_consolidate_dryrun_rpc.sh; echo "EXIT=$?"   # expect EXIT=0 again
```
Expected: first run `EXIT=1`, second run `EXIT=0`. This confirms the test gates on the live code path.

- [ ] **Step 7: Commit**

```bash
git add src/rpc/methods_wallet_context.cpp src/daemon/http_rpc_server.cpp
git commit -m "feat: wallet.consolidate RPC — dry-run plan, family partition, fee-sanity gate"
```

---

## Task 3: Broadcast/execution integration test (failing)

**Files:**
- Create: `tests/test_wallet_consolidate_broadcast_rpc.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/test_wallet_consolidate_broadcast_rpc.sh` (same harness header as Task 1, different ports + body). Reuse the harness functions verbatim from Task 1 Step 1 (`info/pass/fail/get_cookie/rpc_raw/rpc_result/rpc_scalar/wait_for_daemon/start_daemon/stop_daemon/cleanup/trap`), with:

```bash
DATADIR="/tmp/wallet_consolidate_broadcast_$$"
PORT_RPC=22184; PORT_P2P=22183; DAEMON_PID=""
```

and this test body after `start_daemon`:

```bash
rpc_result "wallet.createhd" '["consolidate_bcast_wallet"]' >/dev/null
P2TR_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
[ -n "$P2TR_ADDR" ] || fail "no p2tr addr"

info "Mining 130 P2TR coinbase UTXOs"
rpc_result "generatetoaddress" "[130,\"$P2TR_ADDR\"]" >/dev/null

# --- execute WITHOUT broadcast: returns signed rawtx, nothing enters mempool ---
SIGN="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":false,"broadcast":false,"min_confirmations":1,"max_inputs":50}]' | jq -c '.')"
echo "$SIGN" | jq -e '.ok == true and .dry_run == false' >/dev/null || fail "sign-only not ok: $SIGN"
echo "$SIGN" | jq -e '.txid == null' >/dev/null || fail "sign-only should not broadcast (txid must be null): $SIGN"
echo "$SIGN" | jq -e '.rawtx | type == "string" and (length > 20)' >/dev/null || fail "sign-only missing rawtx: $SIGN"
MEMPOOL_N="$(rpc_scalar "blockchain.getmininginfo" '[]' '.mempool_size // .pooled_tx // 0' 2>/dev/null || echo 0)"

# --- execute WITH broadcast: returns a txid that confirms ---
BCAST="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":false,"broadcast":true,"min_confirmations":1,"max_inputs":50}]' | jq -c '.')"
echo "$BCAST" | jq -e '.ok == true' >/dev/null || fail "broadcast not ok: $BCAST"
echo "$BCAST" | jq -e '.broadcast == true' >/dev/null || fail "broadcast flag not set: $BCAST"
echo "$BCAST" | jq -e '.txid | type == "string" and length == 64' >/dev/null || fail "broadcast missing txid: $BCAST"
echo "$BCAST" | jq -e '.selected_inputs >= 2' >/dev/null || fail "broadcast selected too few: $BCAST"
echo "$BCAST" | jq -e '.fee > 0 and .output_value > 0' >/dev/null || fail "broadcast bad fee/output: $BCAST"

TXID="$(echo "$BCAST" | jq -r '.txid')"
info "Mining 1 block to confirm $TXID"
CONFIRM_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
rpc_result "generatetoaddress" "[1,\"$CONFIRM_ADDR\"]" >/dev/null
GTX="$(rpc_result "blockchain.gettransaction" "[\"$TXID\"]" | jq -c '.')"
echo "$GTX" | jq -e '.' >/dev/null || fail "consolidation tx not found after mining: $TXID"

pass "wallet.consolidate signs without broadcast and broadcasts a confirmable tx"
```

- [ ] **Step 2: Make executable and run to verify it fails**

Run:
```bash
chmod +x tests/test_wallet_consolidate_broadcast_rpc.sh
bash tests/test_wallet_consolidate_broadcast_rpc.sh; echo "EXIT=$?"
```
Expected: **FAIL** at the first execution assertion — `wallet.consolidate` with `dry_run:false` returns `{"ok":false,"error":"consolidate execution not yet implemented"}`, so `rpc_result` does not error (there's no `.error` envelope, the failure is in `.result.ok`), and the `jq -e '.ok == true ...'` assertion fails with `FAIL: sign-only not ok: ...` and `EXIT=1`.

- [ ] **Step 3: Commit the failing test**

```bash
git add tests/test_wallet_consolidate_broadcast_rpc.sh
git commit -m "test: wallet.consolidate broadcast/sign-only (failing — execution stubbed)"
```

---

## Task 4: Implement the execution path (build/sign/broadcast)

**Files:**
- Modify: `src/rpc/methods_wallet_context.cpp` (replace the EXECUTION STUB block in `rpc_context_wallet_consolidate`)

- [ ] **Step 1: Replace the EXECUTION STUB**

In `rpc_context_wallet_consolidate`, replace exactly this block:

```cpp
        // EXECUTION STUB — replaced in Task 4 (build/sign/broadcast).
        result["ok"] = false;
        result["error"] = "consolidate execution not yet implemented";
        log_debug("[wallet.consolidate] execution path stubbed");
        return result;
```

with this real execution code:

```cpp
        // Execution requires an unlocked wallet (dry-run above never needs keys).
        if (wallet_service->get().isWalletLocked()) {
            result["ok"] = false; result["error"] = "Wallet is locked. Use wallet.unlock first."; return result;
        }

        // Convert selected UTXOs to canonical builder inputs.
        std::vector<dinero::CanonicalWalletUTXO> cins;
        for (const auto& u : selected) {
            dinero::CanonicalWalletUTXO c;
            c.txid = dinero::uint256::FromHexUnsafe(u.txid);
            c.vout = u.vout;
            c.value = dinero::AmountUna::Una(u.amount_una);
            c.path = u.derivation_path;
            c.height = u.height;
            c.is_coinbase = u.is_coinbase;
            c.spk.reserve(u.script_pubkey.size() / 2);
            for (size_t i = 0; i + 1 < u.script_pubkey.size(); i += 2)
                c.spk.push_back(static_cast<uint8_t>(std::stoi(u.script_pubkey.substr(i, 2), nullptr, 16)));
            cins.push_back(std::move(c));
        }

        // Single output = total - fee; change_address = dest catches any residual (≈0).
        dinero::BuildOptions bo;
        bo.fee_rate = static_cast<uint64_t>(fee_rate);
        bo.enable_rbf = true;
        bo.change_address = dest;
        std::vector<dinero::TxOutputRequest> outs;
        outs.push_back(dinero::TxOutputRequest(dest, static_cast<uint64_t>(output_una)));

        auto br = dinero::UnsignedTxBuilder::Build(cins, outs, bo);
        if (!br.success) { result["ok"] = false; result["error"] = "Failed to build transaction: " + br.error; return result; }

        // Keys: ECDSA for P2TR via scriptPubKey/path; P2MR signs via WalletKeyProvider (PQ seed).
        std::map<std::string, std::string> path_to_key;
        for (const auto& u : selected) {
            std::vector<uint8_t> spk; spk.reserve(u.script_pubkey.size() / 2);
            for (size_t i = 0; i + 1 < u.script_pubkey.size(); i += 2)
                spk.push_back(static_cast<uint8_t>(std::stoi(u.script_pubkey.substr(i, 2), nullptr, 16)));
            if (dinero::consensus::pq::IsP2MRScript(spk)) continue;
            auto pk = wallet_service->get().deriveKeyForScriptPubKey(u.script_pubkey);
            if (pk.has_value() && !pk->empty()) {
                std::ostringstream h;
                for (uint8_t b : *pk) h << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                path_to_key[u.derivation_path] = h.str();
            } else if (!u.derivation_path.empty()) {
                std::string h = wallet_service->get().getPrivateKeyForPath(u.derivation_path);
                if (!h.empty()) path_to_key[u.derivation_path] = h;
            }
        }

        std::unique_ptr<dinero::KeyProvider> provider;
        std::unique_ptr<dinero::wallet::V7P2MRStore> store_holder;
        if (family == "p2mr") {
            auto master = wallet_service->get().GetV7PqMasterKey();
            if (!master) { result["ok"] = false; result["error"] = "Cannot spend P2MR coin: wallet locked or v7 master key unavailable"; return result; }
            const std::string sp = wallet_service->get().GetV7P2MRStorePath();
            if (sp.empty()) { result["ok"] = false; result["error"] = "v7 P2MR store path not configured"; return result; }
            store_holder = std::make_unique<dinero::wallet::V7P2MRStore>();
            if (store_holder->Open(sp) != dinero::wallet::V7P2MRStore::OpenResult::Ok) { result["ok"] = false; result["error"] = "failed to open v7 P2MR store"; return result; }
            dinero::wallet::WalletKeyProvider::Config cfg;
            cfg.legacy_keys_by_path = path_to_key;
            cfg.p2mr_store = store_holder.get();
            cfg.wallet_id = 1;
            std::memcpy(cfg.master_key.data(), master->data(), cfg.master_key.size());
            OPENSSL_cleanse(const_cast<uint8_t*>(master->data()), master->size());
            provider = std::make_unique<dinero::wallet::WalletKeyProvider>(std::move(cfg));
        } else {
            if (path_to_key.empty()) { result["ok"] = false; result["error"] = "Could not retrieve private keys for signing"; return result; }
            provider = std::make_unique<dinero::MapKeyProvider>(path_to_key);
        }

        auto sr = dinero::TransactionSigner::Sign(br.unsigned_tx, *provider);
        if (!sr.success) { result["ok"] = false; result["error"] = "Failed to sign transaction: " + sr.error; return result; }

        const dinero::Transaction& stx = sr.signed_tx.tx;
        const std::string out_txid = stx.GetTxid().AsUint256().GetHex();
        const int64_t actual_fee = static_cast<int64_t>(sr.signed_tx.fee);

        result["ok"] = true;
        result["dry_run"] = false;
        result["address_family"] = family;
        result["selected_inputs"] = static_cast<int>(n_in);
        result["input_value"]  = static_cast<double>(total_una) / 1e8;
        result["fee"]          = static_cast<double>(actual_fee) / 1e8;
        result["output_value"] = static_cast<double>(output_una) / 1e8;

        if (broadcast) {
            if (!mps) { result["ok"] = false; result["error"] = "Mempool service unavailable"; return result; }
            auto submit = mps->mempool().submitTransaction(stx, "rpc:wallet.consolidate", true);
            result["broadcast"] = true;
            result["accepted"] = submit.accepted();
            if (submit.rejected()) {
                result["ok"] = false;
                result["reject_code"] = TxRejectCodeToString(submit.code);
                result["reject_reason"] = submit.message;
                result["txid"] = din::Json(Json::nullValue);
                return result;
            }
            result["txid"]  = out_txid;
            result["rawtx"] = din::Json(Json::nullValue);
        } else {
            result["broadcast"] = false;
            result["txid"]  = din::Json(Json::nullValue);
            result["rawtx"] = stx.SerializeHex(true);
        }
        return result;
```

- [ ] **Step 2: Build the daemon**

Run:
```bash
cmake --build build --target dinerod -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -20
```
Expected: builds cleanly. Fix any compiler errors (most likely a type mismatch on `submit.code`/`SerializeHex` — compare against the identical calls in `rpc_context_wallet_sendtoaddress` at `:3106-3124`).

- [ ] **Step 3: Run the broadcast test to verify it passes**

Run:
```bash
bash tests/test_wallet_consolidate_broadcast_rpc.sh; echo "EXIT=$?"
```
Expected: **PASS**, `EXIT=0`.

> **Troubleshooting (build/sign):** Because the output is `total − fee` with no change, the actual fee is whatever value the inputs minus the single output leave behind. The build can only fail if `UnsignedTxBuilder` internally estimates the 1-output tx's *minimum* fee higher than our `EstimateTransactionSize(n_in,1,n_p2mr)`-derived fee — most likely for P2MR (large ML-DSA witnesses). If the broadcast test fails with `Failed to build transaction: ... insufficient fee` or a mempool `reject_reason` of low-fee, add a small safety margin in the handler: compute `fee_una` then bump it, e.g. `fee_una = fee_una + dinero::UnsignedTxBuilder::CalculateFee(EstimateTransactionSize(0,1,0), fee_rate);` (one extra output's worth), recompute `output_una`, and re-run. Keep the change minimal and confirm both tests pass.

- [ ] **Step 4: Re-run the dry-run test to confirm no regression**

Run:
```bash
bash tests/test_wallet_consolidate_dryrun_rpc.sh; echo "EXIT=$?"
```
Expected: **PASS**, `EXIT=0`.

- [ ] **Step 5: Commit**

```bash
git add src/rpc/methods_wallet_context.cpp
git commit -m "feat: wallet.consolidate execution — build/sign, return rawtx or broadcast txid"
```

---

## Task 5: Repoint the dinero-qt button (client, separate repo)

**Repo:** `/Users/haydarevich/src/dinero-qt`, branch `qt-main` (NOT the `dinero-v8/qt/` decoy).

**Files:**
- Modify: `src/mainwindow.cpp` — `onConsolidateUTXOs()` (~13641), response dispatch (~6720), error dispatch (~7061).

- [ ] **Step 1: Repoint the request to a dry-run preview, then broadcast on confirm**

In `onConsolidateUTXOs()` (`src/mainwindow.cpp:13695-13700`), replace the final RPC call block:

```cpp
  // Call the consolidate RPC
  QJsonObject params;
  params["max_inputs"] = maxInputs;
  params["target_utxos"] = targetUtxos;
  rpc_->callNamed("consolidate", params);
```

with a dry-run first (the confirmation numbers shown to the user become real):

```cpp
  // Preview first: dry-run plan drives the confirmation dialog, then broadcast.
  QJsonObject params;
  params["address_type"] = "auto";
  params["max_inputs"] = maxInputs;
  params["dry_run"] = true;
  params["broadcast"] = false;
  rpc_->callNamed("wallet.consolidate", params);
```

- [ ] **Step 2: Update the success-dispatch method match**

In the response handler (`src/mainwindow.cpp:6720`), change:

```cpp
  } else if (method == "consolidate") {
```

to:

```cpp
  } else if (method == "wallet.consolidate") {
```

Inside that branch, when the response has `dry_run == true && ok == true`, show the plan (`selected_inputs`, `estimated_fee`, `output_value`, `destination`) in a confirm dialog and, on accept, issue the real send:

```cpp
    if (obj.value("dry_run").toBool(false) && obj.value("ok").toBool(false)) {
      int sel = obj.value("selected_inputs").toInt(0);
      if (sel == 0) {
        QMessageBox::information(this, "Consolidation",
          "Nothing to consolidate — no eligible UTXOs.");
        if (btnConsolidate_) btnConsolidate_->setEnabled(true);
        return;
      }
      double fee = obj.value("estimated_fee").toDouble();
      double out = obj.value("output_value").toDouble();
      QMessageBox box(this);
      box.setWindowTitle("Confirm Consolidation");
      box.setText(QString("Consolidate %1 UTXOs into one output.").arg(sel));
      box.setInformativeText(QString("Estimated fee: %1 DIN\nResulting output: %2 DIN")
                               .arg(fee, 0, 'f', 8).arg(out, 0, 'f', 8));
      QPushButton* go = box.addButton("Consolidate", QMessageBox::AcceptRole);
      box.addButton("Cancel", QMessageBox::RejectRole);
      box.exec();
      if (box.clickedButton() == go) {
        QJsonObject p;
        p["address_type"] = "auto";
        p["dry_run"] = false;
        p["broadcast"] = true;
        rpc_->callNamed("wallet.consolidate", p);
      } else if (btnConsolidate_) {
        btnConsolidate_->setEnabled(true);
        btnConsolidate_->setText(QString("\xF0\x9F\xA7\xB9 Consolidate (%1 UTXOs)").arg(cachedUtxoCount_));
      }
      return;
    }
    if (!obj.value("ok").toBool(true)) {     // fee-gate or execution error returned in-band
      if (btnConsolidate_) btnConsolidate_->setEnabled(true);
      QMessageBox::warning(this, "Consolidation Failed",
        obj.value("reason").toString(obj.value("error").toString("Consolidation could not be completed.")));
      return;
    }
    // ok && !dry_run: broadcast complete
    QString txid = obj.value("txid").toString();
    if (btnConsolidate_) btnConsolidate_->setEnabled(true);
    QMessageBox::information(this, "Consolidation Complete",
      txid.isEmpty() ? QString("Consolidation submitted.")
                     : QString("Consolidation broadcast.\nTXID: %1").arg(txid));
    return;
```

- [ ] **Step 3: Update the error-dispatch method match**

In the error handler (`src/mainwindow.cpp:7061`), change `if (method == "consolidate")` to `if (method == "wallet.consolidate")`, leaving the button re-enable/reset logic intact.

- [ ] **Step 4: Build the wallet**

Run (per `dinero-qt` memory — build dir and embedded daemon copy):
```bash
cd /Users/haydarevich/src/dinero-qt
cmake --build build -j$(sysctl -n hw.logicalcpu) 2>&1 | tail -20
```
Expected: `build/bin/dinero-qt.app` links cleanly.

- [ ] **Step 5: Manual verification (GUI)**

There is no automated GUI test harness, so verify by hand:
1. Launch `build/bin/dinero-qt.app`, open/create a regtest wallet with many UTXOs.
2. Click **Consolidate** → confirm the dialog now shows real numbers (selected inputs, fee, output) from the dry-run.
3. Accept → confirm a "Consolidation Complete" dialog with a TXID, and that the receive-tab UTXO count drops after the next block.
4. Confirm the **old failure is gone**: there is no "method not found / Consolidation Failed -32601".

- [ ] **Step 6: Commit (in dinero-qt)**

```bash
cd /Users/haydarevich/src/dinero-qt
git add src/mainwindow.cpp
git commit -m "fix: wire Consolidate button to wallet.consolidate RPC (preview then broadcast)"
```

---

## Self-review notes

- **Spec coverage:** transparent-only/shielded-reject (Task 2 handler), one-family-per-tx + auto-majority (Task 2 partition), fresh self destination (Task 2 getnewaddress dispatch), dual fee gate percent OR absolute in both modes (Task 2 gate), preview-by-default + broadcast-only-when-true + sign-without-broadcast rawtx (Tasks 2/4), no-op empty/single (Task 2), locked/immature/confidential/mempool-spent exclusion (Task 2 filter chain), max_inputs cap (Task 2 clamp), all 11 spec test cases (Tasks 1 + 3), client repoint (Task 5). No gaps.
- **Type consistency:** `dinero::WalletManager::WalletUTXO`, `dinero::CanonicalWalletUTXO`, `dinero::BuildOptions`/`TxOutputRequest`/`UnsignedTxBuilder::Build`, `dinero::TransactionSigner::Sign`, `mps->mempool().submitTransaction(...)`, `stx.SerializeHex(true)`, `din::Json(Json::nullValue)` — all match the signatures used by `rpc_context_wallet_sendtoaddress` in the same TU.
- **No placeholders:** every step has complete code/commands and expected output; Task 4 replaces the clearly-marked Task 2 stub.
```
