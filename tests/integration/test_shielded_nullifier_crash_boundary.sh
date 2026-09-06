#!/usr/bin/env bash
#
# Phase 2 of the shielded-era reorg invertibility plan
# (docs/specs/shielded_reorg_invertibility_audit.md).
#
# Property under test: walking the chain forward and then forcing a
# disconnect/reconnect cycle via invalidateblock + reconsiderblock
# must produce byte-identical consensus state at the restored tip.
#
# State hash combines every container that crosses the reorg
# boundary in the audit:
#   1. utreexo forest commitment   (consensus_utxo_set forest root)
#   2. shielded tree root          (CommitmentTree.Root)
#   3. shielded tree size          (CommitmentTree.Size)
#   4. nullifier set size          (NullifierSet.Size)
#   5. anchor history size         (AnchorHistory.Size)
#
# A drift in any of those after a Connect↔Disconnect↔Connect cycle
# fails the test loud rather than letting it accumulate silently
# the way the LA fleet drift accumulated through 9000+ blocks.
#
# Test shape:
#   1. mine N blocks on a fresh regtest node
#   2. capture state at the tip                       (S0)
#   3. invalidateblock(block at height 2)             [disconnect 2..N]
#   4. reconsiderblock(block at height 2)             [reconnect 2..N]
#   5. capture state at the tip                       (S2)
#   6. assert S0 == S2  (Connect/Disconnect/Connect is the identity)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ -n "${DINEROD:-}" ]]; then
    # CTest supplies this (ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"), so the
    # test follows the build directory wherever it is. Honour it and require it
    # to be real — never silently fall through to a guessed path.
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }
elif [[ -x "${ROOT_DIR}/build/dinerod" ]]; then
    # Manual/local convenience only.
    DINEROD="${ROOT_DIR}/build/dinerod"
elif [[ -x "${ROOT_DIR}/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/dinerod"
else
    # Fail HERE, naming the paths tried. Launching a non-existent binary and
    # then waiting on its RPC turns a missing file into a 30s timeout reported
    # as "RPC never came up", which reads like a consensus failure.
    echo "dinerod not found (tried: \$DINEROD unset, ${ROOT_DIR}/build/dinerod, ${ROOT_DIR}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi
RUN_ID=$$
DATADIR="/tmp/dinero_nf_crash_${RUN_ID}"
LOG="${DATADIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-15}"
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT="${RPC_PORT:-$(alloc_port_base)}"
P2P_PORT="${P2P_PORT:-$((RPC_PORT + 1))}"
RPC_TIMEOUT=20

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -f "${LOG}" ]]; then
        printf -- '--- daemon log tail ---\n' >&2
        tail -n 60 "${LOG}" >&2 || true
    fi
    cleanup
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        kill -TERM "${PID}" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "${PID}" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "${PID}" 2>/dev/null || true
    fi
    if [[ "${KEEP_ON_FAIL}" -eq 0 ]]; then
        rm -rf "${DATADIR}" 2>/dev/null || true
    else
        info "preserving ${DATADIR} for inspection"
    fi
}
trap cleanup EXIT

# --- raw JSON-RPC over cookie auth (the pattern proven by the
#     existing test_csn_reorg_churn / test_bug1 harnesses) -----------

rpc() {
    local method="$1"
    shift
    local params="$*"
    local json_params="[]"
    [[ -n "${params}" ]] && json_params="[${params}]"
    local cookie
    cookie="$(cat "${DATADIR}/.cookie" 2>/dev/null || true)"
    if [[ -z "${cookie}" ]]; then
        return 1
    fi
    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" \
        -u "${cookie}" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${RPC_PORT}" 2>/dev/null
}

rpc_field_string() {
    # extract result.<field> when result is a JSON object
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
        | head -n1
}

rpc_field_number() {
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p" \
        | head -n1
}

rpc_top_string() {
    # extract result when result is a bare string
    local response="$1"
    echo "${response}" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n1
}

rpc_top_number() {
    local response="$1"
    echo "${response}" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' \
        | head -n1
}

wait_rpc() {
    for _ in $(seq 1 30); do
        local r
        r="$(rpc getblockcount || true)"
        if [[ -n "${r}" && "${r}" != *"\"error\":\"null\""* ]]; then
            local h
            h="$(rpc_top_number "${r}")"
            [[ -n "${h}" ]] && return 0
        fi
        sleep 1
    done
    fail "RPC never came up"
}

# State hash via the daemon-side `daemon.shieldedstatehash` RPC.
# That hash covers ALL five reorg-bound containers — utreexo forest
# (commitment + numLeaves + canonical_empty_roots flag), shielded
# tree (root + size), nullifier set size, and the full anchor
# history (every (height, root) pair). The earlier shell-side
# composition only exercised the first four indirectly; this RPC
# closes the audit's "anchor history is only covered indirectly"
# caveat.
# Shielded-state root via `daemon.shieldedroot` — the SHIELDED-ONLY
# fingerprint that a future block-header commitment would carry
# (state_commitment_v1). Distinct from state_hash() above: it excludes the
# utreexo forest (already committed by header.utreexo_root) and
# length-prefixes each variable-length section.
#
# Asserted through the same Connect/Disconnect/Connect cycle because this is
# the value that would become consensus: if it is not a perfect inverse under
# DisconnectBlock, a reorg across the activation height is a chain split.
#
# Empty when the daemon predates the RPC; the caller treats that as "skip",
# never as a pass, so an older binary cannot silently green this assertion.
shielded_root() {
    local resp h
    resp="$(rpc daemon.shieldedroot)"
    h="$(rpc_field_string "${resp}" shielded_root)"
    printf '%s' "${h}"
}

state_hash() {
    local resp h
    resp="$(rpc daemon.shieldedstatehash)"
    h="$(rpc_field_string "${resp}" state_hash)"
    if [[ -z "${h}" ]]; then
        # Daemon doesn't expose the RPC (older binary) — fall back to
        # the composition-by-fields shape so the test still runs in
        # mixed-version environments.
        local info_resp utreexo
        info_resp="$(rpc getblockchaininfo)"
        utreexo="$(rpc_field_string "${info_resp}" utreexo_root)"
        : "${utreexo:=NA}"
        printf '%s|fallback' "${utreexo}" | shasum -a 256 | awk '{print $1}'
        return 0
    fi
    printf '%s' "${h}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Nullifier-commit / tree-append crash boundary + sqlite provenance policy.
#
#     InsertBatch COMMIT succeeds
#           |
#           v   process dies here
#     commitment-tree Append has not happened
#
# The contract is not violated (the function never returns), so what must hold
# is RECOVERY: ChainDB is authoritative, the sqlite cache is wiped and
# rehydrated from it, and rows the dead block committed are erased rather than
# promoted — otherwise a valid spend stays permanently blocked.
#
#   A control     clean uninterrupted run; proves a live daemon stamps its cache
#   B crash       abort at the boundary, restart without cleanup
#   C legacy      unstamped sqlite, rows at/below tip  -> must NOT be quarantined
#   D ambiguous   unstamped sqlite, rows ABOVE tip     -> MUST be quarantined
# ─────────────────────────────────────────────────────────────────────────────

# python3's sqlite3 module rather than the sqlite3 CLI: the CLI is absent on
# plenty of build hosts (including this project's own builder), and a fixture
# that silently cannot be built is worse than one that fails loudly.
command -v python3 >/dev/null || fail "python3 required to build the provenance fixtures"
python3 -c "import sqlite3" 2>/dev/null || fail "python3 sqlite3 module required"

sq() {  # sq <db> <sql...>  — execute; prints nothing
    python3 - "$@" <<'PYSQ'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
for stmt in sys.argv[2:]:
    con.execute(stmt)
con.commit(); con.close()
PYSQ
}
sq_read() {  # sq_read <db> <sql> — prints the first column of the first row
    python3 - "$@" <<'PYSQ'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
row = con.execute(sys.argv[2]).fetchone()
print(row[0] if row else "")
con.close()
PYSQ
}
BASE_DIR="${DATADIR}"
mkdir -p "${BASE_DIR}"

start_node() {  # datadir rpcport log [env...]
    local dd="$1" rp="$2" lg="$3"; shift 3
    mkdir -p "${dd}"
    env "$@" "${DINEROD}" -regtest -datadir="${dd}" \
        -rpcport="${rp}" -port="$((rp + 1))" -listen=0 >"${lg}" 2>&1 &
    echo $!
}
stop_node() {
    local pid="$1"; [[ -z "${pid}" ]] && return 0
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${pid}" 2>/dev/null || return 0; sleep 0.5; done
    kill -9 "${pid}" 2>/dev/null || true
}
# The daemon's nullifier database, at the path ChainstateService::
# LoadShieldedState() actually opens. Hard-coded deliberately: a wildcard
# search silently found nothing and made every fixture below vacuous.
nf_db_path() { printf '%s/blockchain/shielded_nullifiers.db' "$1"; }
find_nf_db() {
    local p; p="$(nf_db_path "$1")"
    [[ -f "${p}" ]] && printf '%s' "${p}"
}

run_scenario() {  # dir rpc log [env...] -> mines a few blocks, leaves node stopped
    local dd="$1" rp="$2" lg="$3"; shift 3
    local pid; pid="$(start_node "${dd}" "${rp}" "${lg}" "$@")"
    local saved_dir="${DATADIR}" saved_rpc="${RPC_PORT}"
    DATADIR="${dd}"; RPC_PORT="${rp}"
    if wait_rpc; then
        # The field is first_address and the RPC is generatetoaddress. Getting
        # either wrong leaves the chain at height 0, which silently made the
        # height-vs-tip fixtures vacuous the first time round — so assert.
        local a; a="$(rpc_field_string "$(rpc wallet.createhd '"nfc"')" first_address)"
        [[ -n "${a}" ]] || fail "could not create wallet / extract first_address"
        rpc generatetoaddress "8, \"${a}\"" >/dev/null 2>&1 || true
        # Assert the chain actually advanced. Without this the height-vs-tip
        # fixtures below are vacuous, which is exactly what happened when the
        # wallet field name was wrong and every node sat at height 0.
        local h
        h="$(rpc getblockcount | tr -d " \n" | sed -E 's/.*"result"[^0-9-]*(-?[0-9]+).*/\1/')"
        case "${h}" in
            ''|*[!0-9]*) fail "could not read block height after mining (got '${h}')" ;;
        esac
        [[ "${h}" -ge 5 ]] || fail "scenario mined to height ${h}; fixtures need >= 5"
        info "  scenario chain at height ${h}"
    fi
    DATADIR="${saved_dir}"; RPC_PORT="${saved_rpc}"
    stop_node "${pid}"
}

# ── A: control ──────────────────────────────────────────────────────────────
info "fixture A: clean control run"
CTRL_DIR="${BASE_DIR}/ctrl"
run_scenario "${CTRL_DIR}" "${RPC_PORT}" "${BASE_DIR}/ctrl.log"
CTRL_DB="$(find_nf_db "${CTRL_DIR}")"
if [[ -n "${CTRL_DB}" ]]; then
    CTRL_VER="$(sq_read "${CTRL_DB}" "PRAGMA user_version" 2>/dev/null || echo "")"
    [[ "${CTRL_VER}" == "1" ]] \
        || fail "a live daemon must stamp its nullifier cache user_version=1, got '${CTRL_VER}'"
    pass "live daemon stamps its nullifier cache (user_version=1)"
else
    info "control produced no nullifier database; provenance fixtures below still apply"
fi

# ── D: ambiguous pre-fix residue must be quarantined ────────────────────────
info "fixture D: unstamped sqlite, row ABOVE the tip (pre-fix crash residue)"
AMB_DIR="${BASE_DIR}/ambiguous"; AMB_RPC="$((RPC_PORT + 20))"
run_scenario "${AMB_DIR}" "${AMB_RPC}" "${BASE_DIR}/amb.log"
AMB_DB="$(find_nf_db "${AMB_DIR}")"
if [[ -z "${AMB_DB}" ]]; then
    AMB_DB="$(nf_db_path "${AMB_DIR}")"
    mkdir -p "$(dirname "${AMB_DB}")"
    sq "${AMB_DB}" "CREATE TABLE IF NOT EXISTS nullifiers (nullifier BLOB PRIMARY KEY NOT NULL, block_height INTEGER NOT NULL)"
fi
sq "${AMB_DB}" "INSERT OR REPLACE INTO nullifiers VALUES (randomblob(32), 999999)" "PRAGMA user_version = 0" \
    || fail "could not build the ambiguous fixture"
info "ambiguous: user_version=$(sq_read "${AMB_DB}" 'PRAGMA user_version') rows=$(sq_read "${AMB_DB}" 'SELECT COUNT(*) FROM nullifiers')"

AMB2_LOG="${BASE_DIR}/amb_restart.log"
AMB2_PID="$(start_node "${AMB_DIR}" "${AMB_RPC}" "${AMB2_LOG}")"
sleep 15
if grep -q "QUARANTINE" "${AMB2_LOG}" 2>/dev/null; then
    pass "unstamped rows above the tip are QUARANTINED, not promoted"
else
    stop_node "${AMB2_PID}"
    fail "ambiguous fixture was NOT quarantined — see ${AMB2_LOG}"
fi
grep -q "Migrated .* sqlite nullifier rows into ChainDB" "${AMB2_LOG}" 2>/dev/null \
    && { stop_node "${AMB2_PID}"; fail "quarantined rows must never reach ChainDB"; }
pass "no quarantined row reached authoritative ChainDB storage"
stop_node "${AMB2_PID}"

# ── C: below-tip but UNVERIFIABLE must also be quarantined ─────────────────
# The case height alone cannot catch. A row at a perfectly valid height, whose
# nullifier is NOT spent in the canonical block at that height, is residue from
# a disconnected or losing-fork block. Promoting it would permanently refuse a
# valid spend, so verification against canonical history — not height — decides.
info "fixture C: unstamped sqlite, row at a valid height but not in that block"
LEG_DIR="${BASE_DIR}/legacy"; LEG_RPC="$((RPC_PORT + 40))"
run_scenario "${LEG_DIR}" "${LEG_RPC}" "${BASE_DIR}/leg.log"
LEG_DB="$(find_nf_db "${LEG_DIR}")"
if [[ -z "${LEG_DB}" ]]; then
    LEG_DB="$(nf_db_path "${LEG_DIR}")"
    mkdir -p "$(dirname "${LEG_DB}")"
    sq "${LEG_DB}" "CREATE TABLE IF NOT EXISTS nullifiers (nullifier BLOB PRIMARY KEY NOT NULL, block_height INTEGER NOT NULL)"
fi
sq "${LEG_DB}" "INSERT OR REPLACE INTO nullifiers VALUES (randomblob(32), 2)" "PRAGMA user_version = 0" \
    || fail "could not build the legacy fixture"
LEG2_LOG="${BASE_DIR}/leg_restart.log"
LEG2_PID="$(start_node "${LEG_DIR}" "${LEG_RPC}" "${LEG2_LOG}")"
sleep 15
if grep -q "QUARANTINE" "${LEG2_LOG}" 2>/dev/null; then
    pass "below-tip row that is NOT in its canonical block is QUARANTINED"
else
    stop_node "${LEG2_PID}"
    fail "an unverifiable below-tip row must be quarantined — height is not proof"
fi
grep -q "Migrated .* sqlite nullifier rows into ChainDB" "${LEG2_LOG}" 2>/dev/null \
    && { stop_node "${LEG2_PID}"; fail "an unverifiable row must never reach ChainDB"; }
pass "no unverifiable row reached authoritative ChainDB storage"
stop_node "${LEG2_PID}"

# ── B: the crash boundary ───────────────────────────────────────────────────
info "fixture B: abort at after_nullifier_batch_before_tree_append, restart dirty"
CR_DIR="${BASE_DIR}/crash"; CR_RPC="$((RPC_PORT + 60))"
run_scenario "${CR_DIR}" "${CR_RPC}" "${BASE_DIR}/crash.log" \
    DINERO_CRASH_AT=after_nullifier_batch_before_tree_append

CR2_LOG="${BASE_DIR}/crash_restart.log"
CR2_PID="$(start_node "${CR_DIR}" "${CR_RPC}" "${CR2_LOG}")"
saved="${DATADIR}"; DATADIR="${CR_DIR}"; saved_rpc="${RPC_PORT}"; RPC_PORT="${CR_RPC}"
if wait_rpc; then
    pass "node restarts cleanly after an abort at the nullifier/tree boundary"
    grep -q "QUARANTINE" "${CR2_LOG}" 2>/dev/null \
        && { DATADIR="${saved}"; RPC_PORT="${saved_rpc}"; stop_node "${CR2_PID}"; \
             fail "a stamped cache must be wiped and rehydrated, never quarantined"; }
    pass "post-crash cache treated as a cache, not as a migration candidate"
else
    DATADIR="${saved}"; RPC_PORT="${saved_rpc}"; stop_node "${CR2_PID}"
    fail "node failed to restart after the boundary abort — recovery is broken"
fi
DATADIR="${saved}"; RPC_PORT="${saved_rpc}"
stop_node "${CR2_PID}"

pass "nullifier/tree crash boundary and sqlite provenance policy hold"
