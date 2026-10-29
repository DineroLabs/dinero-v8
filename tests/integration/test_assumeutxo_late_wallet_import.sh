#!/usr/bin/env bash
#
# state_commitment_v1: this suite runs DORMANT (explicit override below).
# Its subject is AssumeUTXO snapshot/replay behavior, not commitment
# enforcement -- under regtest's active-at-1 default, export stamps v5 and
# the load-time binding chain requires burial depth the fixture's
# export-at-tip-and-reload shape cannot satisfy. Enforcement itself is
# exercised by the state-commitment suites and the forged-snapshot e2e.
# Regression for the mobile/full-Utreexo ordering that escaped #353:
#
#   1. Load an AssumeUTXO snapshot while the consumer has an unrelated wallet.
#   2. Import the funded mnemonic only AFTER snapshot activation, with rescan
#      disabled, reproducing an already-derived mobile wallet whose DB is empty.
#   3. Restart without history: recovery must remain deferred, not certify zero
#      or trust unbound legacy creation metadata.
#   4. Make the selected archival source available over loopback P2P and invoke
#      the same explicit authenticated recovery RPC as the phone coordinator.
#   5. Require exact inventory, proof coverage, retry/restart persistence and a
#      real spend without another mnemonic import.
#
# Legacy snapshots bind live value/script/outpoint but not every creation flag.
# Recovery therefore needs the authenticated creation bodies too. A snapshot
# scan or successful key import alone is not a wallet-completeness receipt.
set -uo pipefail

# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
BASE_PORT="${BASE_PORT:-$((38600 + ($$ % 500) * 4))}"
SRC_RPC=$((BASE_PORT + 0)); SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
CON_RPC=$((BASE_PORT + 1)); CON_P2P=$((BASE_PORT + 101)); CON_WS=$((BASE_PORT + 201))
BASE_HEIGHT="${BASE_HEIGHT:-120}"
MNEMONIC="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"

WORK="$(mktemp -d -t dinero_late_wallet_import_XXXXXX)"
SRC_DIR="$WORK/source"
CON_DIR="$WORK/consumer"
SNAP="$WORK/snapshot-v4.dat"
HEADERS="$WORK/headers-at-base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }

cookie_for() {
    if [[ -f "$1/.cookie" ]]; then tr -d '\n' < "$1/.cookie"; return 0; fi
    if [[ -f "$1/regtest/.cookie" ]]; then tr -d '\n' < "$1/regtest/.cookie"; return 0; fi
    return 1
}

rpc() {
    local port="$1" datadir="$2" method="$3" params="${4:-[]}" cookie
    cookie="$(cookie_for "$datadir")" || return 1
    curl -fsS --max-time 60 --user "$cookie" \
        -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}

fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for log in "$SRC_DIR"/*.log "$CON_DIR"/*.log; do
        [[ -f "$log" ]] || continue
        printf -- '--- tail %s ---\n' "$log" >&2
        tail -100 "$log" >&2 || true
    done
    KEEP_ON_FAIL=1
    exit 1
}

stop_node() {
    local datadir="$1"
    pkill -f "datadir=$datadir" 2>/dev/null || true
    for _ in $(seq 1 60); do
        pgrep -f "datadir=$datadir" >/dev/null 2>&1 || return 0
        sleep 0.5
    done
    pkill -9 -f "datadir=$datadir" 2>/dev/null || true
}

cleanup() {
    stop_node "$SRC_DIR"
    stop_node "$CON_DIR"
    if [[ "$KEEP_ON_FAIL" == "1" ]]; then
        printf '[INFO] preserving failure workdir: %s\n' "$WORK" >&2
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

start_node() {
    local datadir="$1" rpcport="$2" p2pport="$3" wsport="$4" logfile="$5"
    shift 5
    mkdir -p "$datadir"
    local -a networking=(--p2p.offline=1 --listen=0)
    if [[ "${TEST_HISTORY_NETWORK:-0}" == 1 ]]; then
        networking=(--p2p.offline=0 --listen=1 --bind=127.0.0.1 --dnsseed=0 --fixedseeds=0)
    fi
    "$DINEROD" --regtest --consensus-state-commitment-height=4294967295 --datadir="$datadir" \
        --rpcport="$rpcport" --port="$p2pport" --wallet-socket-port="$wsport" \
        "${networking[@]}" "$@" >"$logfile" 2>&1 &
    for _ in $(seq 1 120); do
        if rpc "$rpcport" "$datadir" getblockcount 2>/dev/null \
            | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    fail "node on RPC port $rpcport did not become ready"
}

[[ -x "$DINEROD" ]] || fail "dinerod is not executable: $DINEROD"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v jq >/dev/null 2>&1 || fail "jq is required"

info "Creating a funded deterministic wallet and V4 snapshot"
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/source.log"
# Preserve the startup wallet; recover this mnemonic under a separate name.
RESTORE_SOURCE="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.restore \
    "{\"name\":\"snapshot-source\",\"mnemonic\":\"$MNEMONIC\",\"rescan\":false}")"
jq -e '.result.success == true' <<<"$RESTORE_SOURCE" >/dev/null \
    || fail "new-name recovery failed: $RESTORE_SOURCE"
IMPORT_SOURCE="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.importmnemonic \
    "{\"mnemonic\":\"$MNEMONIC\",\"rescan\":false,\"initial_address_count\":4}")"
jq -e '.result.success == true and .result.watch_scripts > 0' <<<"$IMPORT_SOURCE" >/dev/null \
    || fail "source mnemonic import failed: $IMPORT_SOURCE"
# Mine to an address already created by importmnemonic. Calling
# getnewaddress here would advance to index N, while the late importer derives
# only 0...(N-1), accidentally testing a discovery-gap mismatch instead of the
# snapshot ordering defect.
OWNER="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.listaddresses '[]' \
    | jq -r '.result[0].address // empty')"
[[ -n "$OWNER" ]] || fail "source wallet returned no imported mining address"
rpc "$SRC_RPC" "$SRC_DIR" generatetoaddress "[$BASE_HEIGHT,\"$OWNER\"]" \
    | jq -e ".result.blocks | length == $BASE_HEIGHT" >/dev/null \
    || fail "source failed to mine snapshot-base chain"
SOURCE_BALANCE="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getbalance '[]' \
    | jq -r '.result.total // ((.result.confirmed // 0) + (.result.immature // 0)) // 0')"
awk -v value="$SOURCE_BALANCE" 'BEGIN { exit !(value > 0) }' \
    || fail "source wallet has no balance before snapshot"
DUMP="$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_written > 0 and .result.base_height == '"$BASE_HEIGHT" <<<"$DUMP" >/dev/null \
    || fail "snapshot export failed: $DUMP"
stop_node "$SRC_DIR"
cp -R "$SRC_DIR/headers" "$HEADERS"

info "Starting consumer with snapshot first and unrelated auto-created wallet"
mkdir -p "$CON_DIR/headers"
cp -R "$HEADERS/." "$CON_DIR/headers/"
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/consumer.log" \
    --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
    --assumeutxo_bg_stall_timeout=3600

# The path configures lifecycle rehydration; first activation is explicit via
# loadtxoutset (the same production RPC used by snapshot bootstrap).  Keeping
# the unrelated auto-created wallet active here is the ordering under test.
LOAD="$(rpc "$CON_RPC" "$CON_DIR" loadtxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_loaded > 0 and .result.base_height == '"$BASE_HEIGHT" <<<"$LOAD" >/dev/null \
    || fail "consumer snapshot activation failed: $LOAD"

SNAP_ACTIVE=0
for _ in $(seq 1 120); do
    STATUS="$(rpc "$CON_RPC" "$CON_DIR" getsnapshotbootstrapstatus '[]' 2>/dev/null || true)"
    if jq -e '.result.snapshot_bootstrap.assumeutxo_active == true and
              .result.snapshot_bootstrap.snapshot_base_height == '"$BASE_HEIGHT" \
              <<<"$STATUS" >/dev/null 2>&1; then
        SNAP_ACTIVE=1
        break
    fi
    sleep 0.5
done
[[ "$SNAP_ACTIVE" == "1" ]] || fail "consumer never activated the snapshot"

PRE_IMPORT_COUNT="$(rpc "$CON_RPC" "$CON_DIR" wallet.listunspent '[]' \
    | jq -r '.result | length')"
[[ "$PRE_IMPORT_COUNT" == "0" ]] \
    || fail "control wallet unexpectedly owns snapshot coins before mnemonic import"
pass "snapshot activated before funded mnemonic import (control wallet has 0 UTXOs)"

info "Creating the affected state: funded mnemonic present, local inventory empty"
# Preserve the startup wallet; recover this mnemonic under a separate name.
RESTORE_CONSUMER="$(rpc "$CON_RPC" "$CON_DIR" wallet.restore \
    "{\"name\":\"snapshot-recovered\",\"mnemonic\":\"$MNEMONIC\",\"rescan\":false}")"
jq -e '.result.success == true' <<<"$RESTORE_CONSUMER" >/dev/null \
    || fail "new-name recovery failed: $RESTORE_CONSUMER"
LATE_IMPORT="$(rpc "$CON_RPC" "$CON_DIR" wallet.importmnemonic \
    "{\"mnemonic\":\"$MNEMONIC\",\"rescan\":false,\"initial_address_count\":4}")"
jq -e '.result.success == true and .result.rescan_triggered == false' \
    <<<"$LATE_IMPORT" >/dev/null \
    || fail "no-rescan mnemonic import failed: $LATE_IMPORT"

PRE_RESTART_BALANCE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
PRE_RESTART_TOTAL="$(jq -r '.result.total // ((.result.confirmed // 0) + (.result.immature // 0)) // 0' \
    <<<"$PRE_RESTART_BALANCE")"
[[ "$PRE_RESTART_TOTAL" == "0" || "$PRE_RESTART_TOTAL" == "0.0" ]] \
    || fail "affected-state control unexpectedly recovered funds before restart: $PRE_RESTART_BALANCE"
pass "control reproduces derived wallet with zero local UTXOs before restart"

stop_node "$CON_DIR"
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/consumer-restart-heal.log" \
    --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
    --assumeutxo_bg_stall_timeout=3600

DEFERRED_BALANCE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
jq -e '.result.total == 0 and .result.utxo_count == 0' <<<"$DEFERRED_BALANCE" >/dev/null \
    || fail "startup published an inventory without its selected history: $DEFERRED_BALANCE"
RECOVERY_PARAMS="{\"walletname\":\"snapshot-recovered\",\"expected_first_address\":\"$OWNER\"}"
DEFERRED="$(rpc "$CON_RPC" "$CON_DIR" wallet.recovertransparentfromsnapshot "$RECOVERY_PARAMS")"
jq -e '(.result.success != true) and
       ((.error.message // .result.error // "") | test("legacy owned coin creation ancestry unavailable|waiting for owned legacy coin creation history"))' <<<"$DEFERRED" >/dev/null \
    || fail "missing selected history was certified: $DEFERRED"
AFTER_REFUSAL="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
jq -e '.result.total == 0 and .result.utxo_count == 0' <<<"$AFTER_REFUSAL" >/dev/null \
    || fail "refused recovery published a partial inventory: $AFTER_REFUSAL"
pass "startup and explicit recovery defer without the required selected history"

# The old startup scanner is intentionally unavailable here. Fetch the actual
# selected creation bodies; do not reimport keys or patch either wallet store.
stop_node "$CON_DIR"
TEST_HISTORY_NETWORK=1 start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/history-source.log"
TEST_HISTORY_NETWORK=1 start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/consumer-history.log" \
    --connect="127.0.0.1:$SRC_P2P" --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
    --assumeutxo_bg_stall_timeout=3600
RECOVERED=0
RECOVERY_DEADLINE=$((SECONDS + 180))
while (( SECONDS < RECOVERY_DEADLINE )); do
    RECOVERY="$(rpc "$CON_RPC" "$CON_DIR" wallet.recovertransparentfromsnapshot "$RECOVERY_PARAMS" 2>/dev/null || true)"
    if jq -e '.result.success == true and .result.whole_wallet_certified == false and
              .result.scope == "transparent-current-coins-and-post-snapshot-history" and
              .result.recovered_height >= '"$BASE_HEIGHT" <<<"$RECOVERY" >/dev/null 2>&1; then
        RECOVERED=1; break
    fi
    sleep 0.5
done
[[ "$RECOVERED" == 1 ]] || fail "authenticated selected-history recovery did not complete: $RECOVERY"
LATE_BALANCE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
TOTAL="$(jq -r '.result.total // 0' <<<"$LATE_BALANCE")"
[[ "$TOTAL" == "$SOURCE_BALANCE" ]] \
    || fail "recovered inventory differs from the funded source (source=$SOURCE_BALANCE): $LATE_BALANCE"
pass "authenticated recovery restored exactly the funded source inventory without reimport"

UNSPENT="$(rpc "$CON_RPC" "$CON_DIR" wallet.listunspent '[1,9999999]')"
jq -e '.result | length > 0 and all(.[]; .spendable == true and .solvable == true)' \
    <<<"$UNSPENT" >/dev/null \
    || fail "recovered snapshot coins are not spendable and solvable: $UNSPENT"
RECORDED="$(jq -r '.result | length' <<<"$UNSPENT")"
pass "recovery restored $RECORDED spendable and solvable snapshot UTXO(s)"

BUNDLE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getproofbundle \
    '[{"min_confirmations":1,"spendable_only":true,"max_utxos":64}]')"
jq -e '.result.utxo_count > 0 and
       ([.result.proofs[]? | select(.success == true)] | length) > 0' \
    <<<"$BUNDLE" >/dev/null \
    || fail "late-import wallet proof bundle is empty: $BUNDLE"
pass "authenticated recovery rebuilt proof coverage for snapshot funds"

# Retry the explicit recovery receipt; it must not add coins/history twice.
RETRY="$(rpc "$CON_RPC" "$CON_DIR" wallet.recovertransparentfromsnapshot "$RECOVERY_PARAMS")"
jq -e '.result.success == true and .result.whole_wallet_certified == false' <<<"$RETRY" >/dev/null \
    || fail "explicit recovery retry did not complete: $RETRY"
RETRY_BALANCE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
jq -e --argjson total "$TOTAL" '.result.total == $total' <<<"$RETRY_BALANCE" >/dev/null \
    || fail "recovery retry changed funded inventory: $RETRY_BALANCE"
pass "explicit recovery retry is idempotent"
stop_node "$SRC_DIR"

# Restart is a separate regression boundary. The UTXO-position index is
# rebuildable/non-persistent, and the wallet snapshot rescan is idempotent.
# A same-process pass alone would miss startup rebuilding the index from
# ChainDB (which intentionally lacks the imported snapshot coin set).
stop_node "$CON_DIR"
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/consumer-restart.log" \
    --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
    --assumeutxo_bg_stall_timeout=3600

REOPENED="$(rpc "$CON_RPC" "$CON_DIR" wallet.getwalletinfo '[]')"
jq -e --arg first "$OWNER" '.result.walletname == "snapshot-recovered" and .result.primary_address == $first' <<<"$REOPENED" >/dev/null \
    || fail "restart changed the recovered wallet identity: $REOPENED"
RESTART_BALANCE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getbalance '[]')"
RESTART_TOTAL="$(jq -r '.result.total // ((.result.confirmed // 0) + (.result.immature // 0)) // 0' \
    <<<"$RESTART_BALANCE")"
[[ "$RESTART_TOTAL" == "$TOTAL" ]] \
    || fail "recovered balance changed across restart (before=$TOTAL after=$RESTART_TOTAL): $RESTART_BALANCE"

RESTART_BUNDLE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getproofbundle \
    '[{"min_confirmations":1,"spendable_only":true,"max_utxos":64}]')"
jq -e '.result.utxo_count > 0 and
       ([.result.proofs[]? | select(.success == true)] | length) > 0' \
    <<<"$RESTART_BUNDLE" >/dev/null \
    || fail "recovered snapshot proofs disappeared across restart: $RESTART_BUNDLE"
pass "balance and proof coverage survive an offline restart"

RECIPIENT="$(rpc "$CON_RPC" "$CON_DIR" wallet.getnewaddress '[]' \
    | jq -r '.result.address // .result // empty')"
[[ -n "$RECIPIENT" ]] || fail "consumer returned no spend recipient"
SEND="$(rpc "$CON_RPC" "$CON_DIR" wallet.sendtoaddress "[\"$RECIPIENT\",1.0]")"
TXID="$(jq -r '.result.txid // .result // empty' <<<"$SEND")"
[[ "$TXID" =~ ^[0-9a-fA-F]{64}$ ]] \
    || fail "wallet could not spend recovered snapshot funds: $SEND"
rpc "$CON_RPC" "$CON_DIR" getrawmempool '[]' \
    | jq -e --arg txid "$TXID" '.result | index($txid) != null' >/dev/null \
    || fail "recovered-funds transaction was not accepted into mempool"
pass "wallet signed and mempool accepted a spend of late-import snapshot funds"

echo "ALL ASSUMEUTXO LATE-WALLET-IMPORT ASSERTIONS PASSED"
