#!/usr/bin/env bash
#
# Differential acceptance parity gate:
# - replays a valid block corpus through baseline + current binaries
# - compares accept/reject outcomes and height progression
# - runs targeted invalid fixture families
#
# Intended usage (manual/release gate):
#   BASELINE_DINEROD=/path/to/dinerod.old BASELINE_SHA=<sha> \
#   CURRENT_DINEROD=/path/to/dinerod.new \
#   tests/test_acceptance_parity.sh --require-baseline
#
# Quick ctest-safe usage (skips when baseline is not provided):
#   ctest -R AcceptanceParity --output-on-failure
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BLOCK_COUNT="${BLOCK_COUNT:-500}"
MAX_MINE_ITERS="${MAX_MINE_ITERS:-2000000}"
STRICT_INVALID_FIXTURES="${STRICT_INVALID_FIXTURES:-0}"
CORPUS_SEED="${CORPUS_SEED:-deterministic-burst-v1}"

BASELINE_BIN="${BASELINE_DINEROD:-}"
CURRENT_BIN="${CURRENT_DINEROD:-${ROOT_DIR}/build/dinerod}"
BASELINE_SHA="${BASELINE_SHA:-unknown}"
BASELINE_LABEL="${BASELINE_LABEL:-unknown}"
CURRENT_SHA="$(git -C "${ROOT_DIR}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"

REQUIRE_BASELINE=0

for arg in "$@"; do
    case "$arg" in
        --require-baseline)
            REQUIRE_BASELINE=1
            ;;
        --help|-h)
            cat <<USAGE
Usage: tests/test_acceptance_parity.sh [--require-baseline]

Environment:
  BASELINE_DINEROD          Path to baseline dinerod (required for real parity)
  BASELINE_SHA              Git SHA for baseline binary (recommended)
  BASELINE_LABEL            Human-readable baseline tag/label (recommended for RC)
  CURRENT_DINEROD           Path to current dinerod (default: ${ROOT_DIR}/build/dinerod)
  BLOCK_COUNT               Number of valid corpus blocks (default: 500)
  MAX_MINE_ITERS            Nonce search iterations for fixture re-mine (default: 2000000)
  STRICT_INVALID_FIXTURES   1=fail if an invalid fixture is accepted by both binaries (default: 0)
  WORKDIR                   Optional output directory (default: /tmp/din_acceptance_parity_<pid>)
USAGE
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [[ -z "${BASELINE_BIN}" ]]; then
    if [[ "${REQUIRE_BASELINE}" == "1" ]]; then
        echo "ERROR: BASELINE_DINEROD is required when --require-baseline is set" >&2
        exit 2
    fi
    echo "SKIP: BASELINE_DINEROD not set; AcceptanceParity gate requires baseline/current binaries"
    echo "BASELINE_SHA=${BASELINE_SHA}"
    echo "CURRENT_SHA=${CURRENT_SHA}"
    echo "CORPUS_SEED=${CORPUS_SEED}"
    exit 0
fi

if [[ ! -x "${BASELINE_BIN}" ]]; then
    echo "ERROR: baseline binary not executable: ${BASELINE_BIN}" >&2
    exit 2
fi

if [[ ! -x "${CURRENT_BIN}" ]]; then
    echo "ERROR: current binary not executable: ${CURRENT_BIN}" >&2
    exit 2
fi

for cmd in curl jq python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: required command not found: $cmd" >&2
        exit 2
    fi
done

WORKDIR="${WORKDIR:-/tmp/din_acceptance_parity_$$}"
mkdir -p "${WORKDIR}"

CORPUS_FILE="${WORKDIR}/valid_blocks.hex"
RESULTS_FILE="${WORKDIR}/parity_results.tsv"
FIXTURE_RESULTS_FILE="${WORKDIR}/fixture_results.tsv"
FIXTURE_DIR="${WORKDIR}/fixtures"
mkdir -p "${FIXTURE_DIR}"

PROD_DIR="${WORKDIR}/producer"
BASE_DIR="${WORKDIR}/baseline"
CUR_DIR="${WORKDIR}/current"

PROD_RPC="${PROD_RPC:-22520}"
PROD_P2P="${PROD_P2P:-22519}"
BASE_RPC="${BASE_RPC:-22530}"
BASE_P2P="${BASE_P2P:-22529}"
CUR_RPC="${CUR_RPC:-22540}"
CUR_P2P="${CUR_P2P:-22539}"

PROD_PID=""
BASE_PID=""
CUR_PID=""
BASELINE_RPCERR_INDEX=0
BASELINE_CRASH_FIXTURE="none"
BASELINE_CRASH_FIXTURE_META="none"

log() {
    echo "[$(date +%H:%M:%S)] $*"
}

node_auth() {
    local datadir="$1"
    local cookie
    cookie="$(cat "${datadir}/.cookie" 2>/dev/null || true)"
    if [[ -z "${cookie}" ]]; then
        return 1
    fi
    if [[ "${cookie}" == *:* ]]; then
        echo "${cookie}"
    else
        echo "__cookie__:${cookie}"
    fi
}

rpc_raw() {
    local datadir="$1" rpcport="$2" method="$3" params="${4:-[]}"
    local auth
    auth="$(node_auth "${datadir}")" || return 1

    curl -sS -X POST "http://127.0.0.1:${rpcport}" \
        -u "${auth}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}"
}

rpc_result() {
    local datadir="$1" rpcport="$2" method="$3" params="${4:-[]}"
    local resp
    resp="$(rpc_raw "${datadir}" "${rpcport}" "${method}" "${params}")" || return 1

    if echo "${resp}" | jq -e '.error != null' >/dev/null 2>&1; then
        return 2
    fi

    echo "${resp}" | jq '.result'
}

rpc_scalar() {
    local datadir="$1" rpcport="$2" method="$3" params="$4" expr="$5"
    rpc_result "${datadir}" "${rpcport}" "${method}" "${params}" | jq -r "${expr}"
}

wait_rpc() {
    local datadir="$1" rpcport="$2"
    local i
    for i in {1..90}; do
        if rpc_raw "${datadir}" "${rpcport}" "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local bin="$1" datadir="$2" rpcport="$3" p2pport="$4" pid_var="$5"

    rm -rf "${datadir}"
    mkdir -p "${datadir}"

    "${bin}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpcport}" \
        --port="${p2pport}" \
        --debug \
        > "${datadir}/daemon.log" 2>&1 &

    local pid=$!
    if ! wait_rpc "${datadir}" "${rpcport}"; then
        echo "ERROR: failed to start node rpc=${rpcport} datadir=${datadir}" >&2
        return 1
    fi
    printf -v "${pid_var}" "%s" "${pid}"
}

stop_pid() {
    local pid="$1"
    if [[ -n "${pid}" ]]; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}

stop_nodes() {
    stop_pid "${PROD_PID}"
    stop_pid "${BASE_PID}"
    stop_pid "${CUR_PID}"
    PROD_PID=""
    BASE_PID=""
    CUR_PID=""
}

cleanup() {
    stop_nodes
}
trap cleanup EXIT

submit_with_height_eval() {
    local datadir="$1" rpcport="$2" hex="$3"

    local before="0"
    before="$(rpc_scalar "${datadir}" "${rpcport}" "getblockcount" "[]" '.' 2>/dev/null || echo "RPC_DOWN")"
    if [[ "${before}" == "RPC_DOWN" ]]; then
        echo "RPCERR|0|0|rpc_down"
        return 0
    fi

    local resp
    if ! resp="$(rpc_raw "${datadir}" "${rpcport}" "blockchain.submitblock" "[\"${hex}\"]" 2>/dev/null)"; then
        echo "RPCERR|${before}|${before}|rpc_submit_failed"
        return 0
    fi

    local after="${before}"
    after="$(rpc_scalar "${datadir}" "${rpcport}" "getblockcount" "[]" '.' 2>/dev/null || echo "${before}")"

    if echo "${resp}" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "${resp}" | jq -r '.error.message // (.error|tostring)')"
        echo "RPCERR|${before}|${after}|${emsg}"
        return 0
    fi

    local result_str
    result_str="$(echo "${resp}" | jq -r 'if .result == null then "null" else (.result|tostring) end')"

    if [[ "${after}" -gt "${before}" ]]; then
        echo "ACCEPT|${before}|${after}|${result_str}"
        return 0
    fi

    if [[ "${result_str}" == "null" ]]; then
        echo "REJECT|${before}|${after}|no_connect"
    else
        echo "REJECT|${before}|${after}|${result_str}"
    fi
}

generate_corpus() {
    log "Starting producer node"
    start_node "${CURRENT_BIN}" "${PROD_DIR}" "${PROD_RPC}" "${PROD_P2P}" PROD_PID

    rpc_result "${PROD_DIR}" "${PROD_RPC}" "wallet.createhd" '["parity"]' >/dev/null 2>&1 || true
    local miner_addr
    miner_addr="$(rpc_scalar "${PROD_DIR}" "${PROD_RPC}" "wallet.getnewaddress" "[]" '.address')"

    : > "${CORPUS_FILE}"
    local i
    for ((i=1; i<=BLOCK_COUNT; i++)); do
        if (( i > 120 && i % 10 == 0 )); then
            local _s
            for _s in 1 2 3; do
                local dest
                dest="$(rpc_scalar "${PROD_DIR}" "${PROD_RPC}" "wallet.getnewaddress" "[]" '.address')"
                rpc_result "${PROD_DIR}" "${PROD_RPC}" "wallet.sendtoaddress" "[\"${dest}\",0.5,\"\",\"\",true]" >/dev/null 2>&1 || true
            done
        fi

        rpc_result "${PROD_DIR}" "${PROD_RPC}" "generatetoaddress" "[1,\"${miner_addr}\"]" >/dev/null
        local bhash bhex
        bhash="$(rpc_scalar "${PROD_DIR}" "${PROD_RPC}" "getbestblockhash" "[]" '.')"
        bhex="$(rpc_result "${PROD_DIR}" "${PROD_RPC}" "getblock" "[\"${bhash}\",0]" | jq -r 'if type=="object" then (.hex // "") else . end')"

        if [[ -z "${bhex}" || "${bhex}" == "null" ]]; then
            echo "ERROR: failed to fetch block hex at corpus height ${i}" >&2
            return 1
        fi

        echo "${bhex}" >> "${CORPUS_FILE}"

        if (( i % 100 == 0 )); then
            log "Corpus generation progress: ${i}/${BLOCK_COUNT}"
        fi
    done

    local prod_h
    prod_h="$(rpc_scalar "${PROD_DIR}" "${PROD_RPC}" "getblockcount" "[]" '.')"
    log "Producer final height: ${prod_h}"
}

find_first_multi_tx_index() {
    python3 - "${CORPUS_FILE}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.exists():
    print("-1")
    raise SystemExit(0)

def read_varint(b, off):
    fb = b[off]
    off += 1
    if fb < 0xfd:
        return fb, off
    if fb == 0xfd:
        return int.from_bytes(b[off:off+2], 'little'), off + 2
    if fb == 0xfe:
        return int.from_bytes(b[off:off+4], 'little'), off + 4
    return int.from_bytes(b[off:off+8], 'little'), off + 8

for idx, line in enumerate(path.read_text().splitlines(), start=1):
    line = line.strip()
    if not line:
        continue
    b = bytes.fromhex(line)
    if len(b) < 129:
        continue
    txc, _ = read_varint(b, 128)
    if txc > 1:
        print(idx)
        raise SystemExit(0)

print("-1")
PY
}

make_fixture_hex() {
    local mode="$1" input_hex="$2" output_file="$3"

    python3 - "${mode}" "${MAX_MINE_ITERS}" "${input_hex}" <<'PY' > "${output_file}"
import hashlib
import sys

mode = sys.argv[1]
max_iters = int(sys.argv[2])
hex_in = sys.argv[3].strip()
if not hex_in:
    raise SystemExit("empty input hex")

def dsha(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()

def read_varint(buf: bytes, off: int):
    if off >= len(buf):
        raise ValueError("varint out of bounds")
    fb = buf[off]
    off += 1
    if fb < 0xfd:
        return fb, off
    if fb == 0xfd:
        if off + 2 > len(buf):
            raise ValueError("varint16 out of bounds")
        return int.from_bytes(buf[off:off+2], 'little'), off + 2
    if fb == 0xfe:
        if off + 4 > len(buf):
            raise ValueError("varint32 out of bounds")
        return int.from_bytes(buf[off:off+4], 'little'), off + 4
    if off + 8 > len(buf):
        raise ValueError("varint64 out of bounds")
    return int.from_bytes(buf[off:off+8], 'little'), off + 8

def enc_varint(v: int) -> bytes:
    if v < 0xfd:
        return bytes([v])
    if v <= 0xffff:
        return b'\xfd' + v.to_bytes(2, 'little')
    if v <= 0xffffffff:
        return b'\xfe' + v.to_bytes(4, 'little')
    return b'\xff' + v.to_bytes(8, 'little')

def parse_tx(buf: bytes, off: int):
    start = off
    if off + 4 > len(buf):
        raise ValueError("tx missing version")
    off += 4

    has_witness = False
    if off + 2 <= len(buf) and buf[off] == 0x00 and buf[off + 1] != 0x00:
        has_witness = True
        off += 2

    vin, off = read_varint(buf, off)
    for _ in range(vin):
        if off + 36 > len(buf):
            raise ValueError("vin out of bounds")
        off += 36
        script_len, off = read_varint(buf, off)
        if off + script_len + 4 > len(buf):
            raise ValueError("vin script/sequence out of bounds")
        off += script_len + 4

    vout, off = read_varint(buf, off)
    for _ in range(vout):
        if off + 8 > len(buf):
            raise ValueError("vout value out of bounds")
        off += 8
        spk_len, off = read_varint(buf, off)
        if off + spk_len > len(buf):
            raise ValueError("vout script out of bounds")
        off += spk_len

    if has_witness:
        for _ in range(vin):
            items, off = read_varint(buf, off)
            for _ in range(items):
                item_len, off = read_varint(buf, off)
                if off + item_len > len(buf):
                    raise ValueError("witness item out of bounds")
                off += item_len

    if off + 4 > len(buf):
        raise ValueError("tx missing locktime")
    off += 4

    return off, bytes(buf[start:off])

def strip_witness(tx: bytes) -> bytes:
    off = 0
    if len(tx) < 4:
        raise ValueError("short tx")
    version = tx[0:4]
    off = 4

    has_witness = False
    if off + 2 <= len(tx) and tx[off] == 0x00 and tx[off + 1] != 0x00:
        has_witness = True
        off += 2

    if not has_witness:
        return tx

    vin_start = off
    vin, off = read_varint(tx, off)
    for _ in range(vin):
        off += 36
        script_len, off = read_varint(tx, off)
        off += script_len + 4
    vin_end = off

    vout_start = off
    vout, off = read_varint(tx, off)
    for _ in range(vout):
        off += 8
        spk_len, off = read_varint(tx, off)
        off += spk_len
    vout_end = off

    for _ in range(vin):
        items, off = read_varint(tx, off)
        for _ in range(items):
            item_len, off = read_varint(tx, off)
            off += item_len

    locktime = tx[off:off+4]
    if len(locktime) != 4:
        raise ValueError("locktime missing")

    return version + tx[vin_start:vin_end] + tx[vout_start:vout_end] + locktime

def txid_raw(tx: bytes) -> bytes:
    return dsha(strip_witness(tx))

def merkle_root_raw(txs):
    if not txs:
        return b"\x00" * 32
    layer = [txid_raw(t) for t in txs]
    while len(layer) > 1:
        nxt = []
        i = 0
        while i < len(layer):
            left = layer[i]
            right = layer[i+1] if i + 1 < len(layer) else left
            nxt.append(dsha(left + right))
            i += 2
        layer = nxt
    return layer[0]

def target_from_bits(bits: int) -> int:
    exp = (bits >> 24) & 0xff
    mant = bits & 0x007fffff
    if bits & 0x00800000:
        raise ValueError("negative compact target not supported")
    if exp <= 3:
        return mant >> (8 * (3 - exp))
    return mant << (8 * (exp - 3))

def mine_nonce(header: bytearray, iters: int):
    bits = int.from_bytes(header[108:112], 'little')
    target = target_from_bits(bits)
    if target <= 0:
        raise ValueError("invalid target")

    start_nonce = int.from_bytes(header[112:116], 'little')
    for i in range(iters):
        nonce = (start_nonce + i) & 0xffffffff
        header[112:116] = nonce.to_bytes(4, 'little')
        h = dsha(bytes(header))
        if int.from_bytes(h, 'big') <= target:
            return nonce
    return None

def parse_block(block_bytes: bytes):
    if len(block_bytes) < 129:
        raise ValueError("short block")

    header = bytearray(block_bytes[:128])
    off = 128
    tx_count, off = read_varint(block_bytes, off)

    txs = []
    for _ in range(tx_count):
        off2, tx = parse_tx(block_bytes, off)
        txs.append(bytearray(tx))
        off = off2

    tail = bytearray(block_bytes[off:])
    return header, txs, tail

def serialize_block(header: bytearray, txs, tail: bytearray) -> bytes:
    out = bytearray()
    out += header
    out += enc_varint(len(txs))
    for tx in txs:
        out += tx
    out += tail
    return bytes(out)

def mutate_coinbase_height(tx: bytearray) -> bytearray:
    b = bytearray(tx)
    off = 4

    if off + 2 <= len(b) and b[off] == 0x00 and b[off + 1] != 0x00:
        off += 2

    vin, off = read_varint(b, off)
    if vin < 1:
        raise ValueError("coinbase has no inputs")

    off += 32 + 4
    script_len, off = read_varint(b, off)

    if script_len < 2 or off + script_len > len(b):
        raise ValueError("coinbase script too short")

    script_start = off
    script0 = b[script_start]

    if script0 > 0 and (1 + script0) <= script_len:
        pos = script_start + 1
    else:
        pos = script_start + min(1, script_len - 1)

    b[pos] ^= 0x01
    return b

def mutate_first_witness_byte(tx: bytearray) -> bytearray:
    b = bytearray(tx)
    off = 4

    has_witness = False
    if off + 2 <= len(b) and b[off] == 0x00 and b[off + 1] != 0x00:
        has_witness = True
        off += 2

    if not has_witness:
        raise ValueError("tx has no witness")

    vin, off = read_varint(b, off)
    for _ in range(vin):
        off += 32 + 4
        script_len, off = read_varint(b, off)
        off += script_len + 4

    vout, off = read_varint(b, off)
    for _ in range(vout):
        off += 8
        spk_len, off = read_varint(b, off)
        off += spk_len

    mutated = False
    for _ in range(vin):
        items, off = read_varint(b, off)
        for _ in range(items):
            item_len, off = read_varint(b, off)
            if item_len > 0 and not mutated:
                b[off] ^= 0x01
                mutated = True
            off += item_len

    if not mutated:
        raise ValueError("no non-empty witness item found")

    return b

blk = bytes.fromhex(hex_in)
header, txs, tail = parse_block(blk)

if mode == "bad_diffbits":
    bits = int.from_bytes(header[108:112], 'little')
    bits ^= 0x00010000
    header[108:112] = bits.to_bytes(4, 'little')
elif mode == "bad_cb_height":
    txs[0] = mutate_coinbase_height(txs[0])
    header[36:68] = merkle_root_raw([bytes(t) for t in txs])
    if mine_nonce(header, max_iters) is None:
        raise SystemExit("mine failed for bad_cb_height")
elif mode == "duplicate_tx":
    if len(txs) < 2:
        raise SystemExit("duplicate_tx requires block with >=2 tx")
    txs.append(bytearray(txs[1]))
    header[36:68] = merkle_root_raw([bytes(t) for t in txs])
    if mine_nonce(header, max_iters) is None:
        raise SystemExit("mine failed for duplicate_tx")
elif mode == "taproot_witness_fail":
    target_idx = -1
    for i in range(1, len(txs)):
        try:
            _ = mutate_first_witness_byte(txs[i])
            target_idx = i
            break
        except Exception:
            continue
    if target_idx < 0:
        raise SystemExit("taproot_witness_fail requires a non-coinbase tx with witness")
    txs[target_idx] = mutate_first_witness_byte(txs[target_idx])
    header[36:68] = merkle_root_raw([bytes(t) for t in txs])
    if mine_nonce(header, max_iters) is None:
        raise SystemExit("mine failed for taproot_witness_fail")
else:
    raise SystemExit(f"unknown mode: {mode}")

out = serialize_block(header, txs, tail)
print(out.hex())
PY
}

run_valid_replay() {
    : > "${RESULTS_FILE}"
    echo -e "idx\tbase_status\tbase_before\tbase_after\tbase_reason\tcur_status\tcur_before\tcur_after\tcur_reason" >> "${RESULTS_FILE}"

    local idx=0
    VALID_ACCEPTED_BASE=0
    VALID_REJECTED_BASE=0
    VALID_ACCEPTED_CUR=0
    VALID_REJECTED_CUR=0
    VALID_MISMATCHES=0
    FIRST_VALID_DIVERGENCE=""

    while IFS= read -r hex; do
        [[ -z "${hex}" ]] && continue
        idx=$((idx + 1))

        local br cr
        br="$(submit_with_height_eval "${BASE_DIR}" "${BASE_RPC}" "${hex}")"
        cr="$(submit_with_height_eval "${CUR_DIR}" "${CUR_RPC}" "${hex}")"

        local b_status b_before b_after b_reason
        local c_status c_before c_after c_reason

        IFS='|' read -r b_status b_before b_after b_reason <<< "${br}"
        IFS='|' read -r c_status c_before c_after c_reason <<< "${cr}"

        if [[ "${b_status}" == "ACCEPT" ]]; then
            VALID_ACCEPTED_BASE=$((VALID_ACCEPTED_BASE + 1))
        else
            VALID_REJECTED_BASE=$((VALID_REJECTED_BASE + 1))
        fi

        if [[ "${b_status}" == "RPCERR" && "${BASELINE_RPCERR_INDEX}" -eq 0 ]]; then
            BASELINE_RPCERR_INDEX="${idx}"
            BASELINE_CRASH_FIXTURE="${WORKDIR}/crash_baseline_block_$(printf "%04d" "${idx}").hex"
            echo "${hex}" > "${BASELINE_CRASH_FIXTURE}"
        fi

        if [[ "${c_status}" == "ACCEPT" ]]; then
            VALID_ACCEPTED_CUR=$((VALID_ACCEPTED_CUR + 1))
        else
            VALID_REJECTED_CUR=$((VALID_REJECTED_CUR + 1))
        fi

        if [[ "${b_status}" != "${c_status}" ]]; then
            VALID_MISMATCHES=$((VALID_MISMATCHES + 1))
            if [[ -z "${FIRST_VALID_DIVERGENCE}" ]]; then
                FIRST_VALID_DIVERGENCE="idx=${idx} baseline=${b_status}:${b_reason} current=${c_status}:${c_reason}"
            fi
        fi

        echo -e "${idx}\t${b_status}\t${b_before}\t${b_after}\t${b_reason}\t${c_status}\t${c_before}\t${c_after}\t${c_reason}" >> "${RESULTS_FILE}"

        if (( idx % 100 == 0 )); then
            log "Replay progress: ${idx}/${BLOCK_COUNT}"
        fi
    done < "${CORPUS_FILE}"

    if [[ "${BASELINE_RPCERR_INDEX}" -gt 0 ]]; then
        BASELINE_CRASH_FIXTURE_META="${WORKDIR}/crash_baseline_block_$(printf "%04d" "${BASELINE_RPCERR_INDEX}").meta"
        cat > "${BASELINE_CRASH_FIXTURE_META}" <<EOF
BASELINE_LABEL=${BASELINE_LABEL}
BASELINE_SHA=${BASELINE_SHA}
CURRENT_SHA=${CURRENT_SHA}
CORPUS_SEED=${CORPUS_SEED}
CORPUS_FILE=${CORPUS_FILE}
CRASH_INDEX=${BASELINE_RPCERR_INDEX}
CRASH_BLOCK_HEX_FILE=${BASELINE_CRASH_FIXTURE}
EOF
    fi

    BASE_FINAL_HEIGHT="$(rpc_scalar "${BASE_DIR}" "${BASE_RPC}" "getblockcount" "[]" '.' || echo RPC_FAIL)"
    CUR_FINAL_HEIGHT="$(rpc_scalar "${CUR_DIR}" "${CUR_RPC}" "getblockcount" "[]" '.' || echo RPC_FAIL)"
}

replay_prefix_or_fail() {
    local datadir="$1" rpcport="$2" prefix_blocks="$3"

    local idx=0
    while IFS= read -r hex; do
        [[ -z "${hex}" ]] && continue
        idx=$((idx + 1))
        if (( idx > prefix_blocks )); then
            break
        fi

        local ev status before after reason
        ev="$(submit_with_height_eval "${datadir}" "${rpcport}" "${hex}")"
        IFS='|' read -r status before after reason <<< "${ev}"

        if [[ "${status}" != "ACCEPT" ]]; then
            echo "ERROR: prefix replay failed at idx=${idx} status=${status} reason=${reason}" >&2
            return 1
        fi
    done < "${CORPUS_FILE}"

    return 0
}

run_fixture_case() {
    local name="$1" fixture_hex="$2" base_index="$3" expect_reject="$4"

    stop_pid "${BASE_PID}"
    stop_pid "${CUR_PID}"
    BASE_PID=""
    CUR_PID=""

    start_node "${BASELINE_BIN}" "${BASE_DIR}" "${BASE_RPC}" "${BASE_P2P}" BASE_PID
    start_node "${CURRENT_BIN}" "${CUR_DIR}" "${CUR_RPC}" "${CUR_P2P}" CUR_PID

    local prefix=$((base_index - 1))
    if (( prefix > 0 )); then
        replay_prefix_or_fail "${BASE_DIR}" "${BASE_RPC}" "${prefix}"
        replay_prefix_or_fail "${CUR_DIR}" "${CUR_RPC}" "${prefix}"
    fi

    local b_eval c_eval
    b_eval="$(submit_with_height_eval "${BASE_DIR}" "${BASE_RPC}" "${fixture_hex}")"
    c_eval="$(submit_with_height_eval "${CUR_DIR}" "${CUR_RPC}" "${fixture_hex}")"

    local b_status b_before b_after b_reason
    local c_status c_before c_after c_reason

    IFS='|' read -r b_status b_before b_after b_reason <<< "${b_eval}"
    IFS='|' read -r c_status c_before c_after c_reason <<< "${c_eval}"

    echo -e "${name}\t${base_index}\t${b_status}\t${b_before}\t${b_after}\t${b_reason}\t${c_status}\t${c_before}\t${c_after}\t${c_reason}" >> "${FIXTURE_RESULTS_FILE}"

    if [[ "${b_status}" != "${c_status}" ]]; then
        FIXTURE_MISMATCHES=$((FIXTURE_MISMATCHES + 1))
        if [[ -z "${FIRST_FIXTURE_DIVERGENCE}" ]]; then
            FIRST_FIXTURE_DIVERGENCE="fixture=${name} baseline=${b_status}:${b_reason} current=${c_status}:${c_reason}"
        fi
    fi

    if [[ "${expect_reject}" == "1" ]]; then
        if [[ "${b_status}" == "ACCEPT" && "${c_status}" == "ACCEPT" ]]; then
            INVALID_ACCEPTED_BOTH=$((INVALID_ACCEPTED_BOTH + 1))
        fi
    fi
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

log "Generating valid corpus (${BLOCK_COUNT} blocks)"
generate_corpus

SAMPLE_BLOCK_HEX="$(sed -n '1p' "${CORPUS_FILE}")"
if [[ -z "${SAMPLE_BLOCK_HEX}" ]]; then
    echo "ERROR: empty corpus" >&2
    exit 1
fi

MULTI_TX_INDEX="$(find_first_multi_tx_index)"
if [[ "${MULTI_TX_INDEX}" == "-1" ]]; then
    echo "ERROR: corpus did not contain any multi-transaction block" >&2
    exit 1
fi

MULTI_BLOCK_HEX="$(sed -n "${MULTI_TX_INDEX}p" "${CORPUS_FILE}")"
if [[ -z "${MULTI_BLOCK_HEX}" ]]; then
    echo "ERROR: failed to read multi-transaction block at index ${MULTI_TX_INDEX}" >&2
    exit 1
fi

log "Starting replay nodes"
start_node "${BASELINE_BIN}" "${BASE_DIR}" "${BASE_RPC}" "${BASE_P2P}" BASE_PID
start_node "${CURRENT_BIN}" "${CUR_DIR}" "${CUR_RPC}" "${CUR_P2P}" CUR_PID

log "Replaying corpus into baseline/current"
run_valid_replay

# Capture replay-tip identity/work before fixture replays mutate per-node state.
VALID_TIP_HASH_BASELINE="$(rpc_scalar "${BASE_DIR}" "${BASE_RPC}" "getbestblockhash" "[]" '.' 2>/dev/null || echo NA)"
VALID_TIP_HASH_CURRENT="$(rpc_scalar "${CUR_DIR}" "${CUR_RPC}" "getbestblockhash" "[]" '.' 2>/dev/null || echo NA)"

VALID_TIP_CHAINWORK_BASELINE="$(
    rpc_result "${BASE_DIR}" "${BASE_RPC}" "getblockchaininfo" "[]" 2>/dev/null | jq -r '.chainwork // "NA"' 2>/dev/null || echo NA
)"
VALID_TIP_CHAINWORK_CURRENT="$(
    rpc_result "${CUR_DIR}" "${CUR_RPC}" "getblockchaininfo" "[]" 2>/dev/null | jq -r '.chainwork // "NA"' 2>/dev/null || echo NA
)"

# Build fixture hex files
BAD_DIFFBITS_FILE="${FIXTURE_DIR}/bad_diffbits.hex"
BAD_CB_HEIGHT_FILE="${FIXTURE_DIR}/bad_cb_height.hex"
DUPLICATE_TX_FILE="${FIXTURE_DIR}/duplicate_tx.hex"
TAPROOT_FAIL_FILE="${FIXTURE_DIR}/taproot_witness_fail.hex"

make_fixture_hex "bad_diffbits" "${SAMPLE_BLOCK_HEX}" "${BAD_DIFFBITS_FILE}"
make_fixture_hex "bad_cb_height" "${SAMPLE_BLOCK_HEX}" "${BAD_CB_HEIGHT_FILE}"
make_fixture_hex "duplicate_tx" "${MULTI_BLOCK_HEX}" "${DUPLICATE_TX_FILE}"
make_fixture_hex "taproot_witness_fail" "${MULTI_BLOCK_HEX}" "${TAPROOT_FAIL_FILE}"

# Run fixture parity
: > "${FIXTURE_RESULTS_FILE}"
echo -e "fixture\tbase_index\tbase_status\tbase_before\tbase_after\tbase_reason\tcur_status\tcur_before\tcur_after\tcur_reason" >> "${FIXTURE_RESULTS_FILE}"

FIXTURE_MISMATCHES=0
FIRST_FIXTURE_DIVERGENCE=""
INVALID_ACCEPTED_BOTH=0

log "Running invalid fixture parity checks"
run_fixture_case "bad_diffbits" "$(cat "${BAD_DIFFBITS_FILE}")" 1 1
run_fixture_case "bad_cb_height" "$(cat "${BAD_CB_HEIGHT_FILE}")" 1 1
run_fixture_case "duplicate_tx" "$(cat "${DUPLICATE_TX_FILE}")" "${MULTI_TX_INDEX}" 1
run_fixture_case "taproot_witness_fail" "$(cat "${TAPROOT_FAIL_FILE}")" "${MULTI_TX_INDEX}" 1

FAILURES=0
CHAINWORK_MISMATCH_ON_EQUAL_HASH=0

if [[ "${VALID_MISMATCHES}" -gt 0 ]]; then
    FAILURES=$((FAILURES + 1))
fi

if [[ "${FIXTURE_MISMATCHES}" -gt 0 ]]; then
    FAILURES=$((FAILURES + 1))
fi

if [[ "${BASE_FINAL_HEIGHT}" != "${CUR_FINAL_HEIGHT}" ]]; then
    FAILURES=$((FAILURES + 1))
fi

# Defensive invariant:
# If both nodes report the same replay tip hash, cumulative work must match.
if [[ "${VALID_TIP_HASH_BASELINE}" == "${VALID_TIP_HASH_CURRENT}" ]]; then
    if [[ "${VALID_TIP_CHAINWORK_BASELINE}" != "NA" && "${VALID_TIP_CHAINWORK_CURRENT}" != "NA" ]]; then
        if [[ "${VALID_TIP_CHAINWORK_BASELINE}" != "${VALID_TIP_CHAINWORK_CURRENT}" ]]; then
            CHAINWORK_MISMATCH_ON_EQUAL_HASH=1
            FAILURES=$((FAILURES + 1))
        fi
    fi
fi

if [[ "${STRICT_INVALID_FIXTURES}" == "1" && "${INVALID_ACCEPTED_BOTH}" -gt 0 ]]; then
    FAILURES=$((FAILURES + 1))
fi

if [[ -z "${FIRST_VALID_DIVERGENCE}" ]]; then
    FIRST_VALID_DIVERGENCE="none"
fi
if [[ -z "${FIRST_FIXTURE_DIVERGENCE}" ]]; then
    FIRST_FIXTURE_DIVERGENCE="none"
fi

if [[ "${INVALID_ACCEPTED_BOTH}" -gt 0 ]]; then
    log "NOTE: ${INVALID_ACCEPTED_BOTH} invalid fixtures were accepted by both binaries"
fi
if [[ "${CHAINWORK_MISMATCH_ON_EQUAL_HASH}" -eq 1 ]]; then
    log "ERROR: replay tip hash matched but chainwork differed (baseline=${VALID_TIP_CHAINWORK_BASELINE}, current=${VALID_TIP_CHAINWORK_CURRENT})"
fi

cat <<SUMMARY
BASELINE_LABEL=${BASELINE_LABEL}
BASELINE_SHA=${BASELINE_SHA}
CURRENT_SHA=${CURRENT_SHA}
CORPUS_SEED=${CORPUS_SEED}
CORPUS_FILE=${CORPUS_FILE}
BASELINE_RPCERR_INDEX=${BASELINE_RPCERR_INDEX}
BASELINE_CRASH_FIXTURE=${BASELINE_CRASH_FIXTURE}
BASELINE_CRASH_FIXTURE_META=${BASELINE_CRASH_FIXTURE_META}
VALID_BLOCKS=${BLOCK_COUNT}
VALID_ACCEPTED_BASELINE=${VALID_ACCEPTED_BASE}
VALID_REJECTED_BASELINE=${VALID_REJECTED_BASE}
VALID_ACCEPTED_CURRENT=${VALID_ACCEPTED_CUR}
VALID_REJECTED_CURRENT=${VALID_REJECTED_CUR}
FINAL_HEIGHT_BASELINE=${BASE_FINAL_HEIGHT}
FINAL_HEIGHT_CURRENT=${CUR_FINAL_HEIGHT}
REPLAY_TIP_HASH_BASELINE=${VALID_TIP_HASH_BASELINE}
REPLAY_TIP_HASH_CURRENT=${VALID_TIP_HASH_CURRENT}
REPLAY_TIP_CHAINWORK_BASELINE=${VALID_TIP_CHAINWORK_BASELINE}
REPLAY_TIP_CHAINWORK_CURRENT=${VALID_TIP_CHAINWORK_CURRENT}
CHAINWORK_MISMATCH_ON_EQUAL_HASH=${CHAINWORK_MISMATCH_ON_EQUAL_HASH}
VALID_MISMATCHES=${VALID_MISMATCHES}
FIRST_VALID_DIVERGENCE=${FIRST_VALID_DIVERGENCE}
FIXTURE_MISMATCHES=${FIXTURE_MISMATCHES}
FIRST_FIXTURE_DIVERGENCE=${FIRST_FIXTURE_DIVERGENCE}
INVALID_FIXTURES_ACCEPTED_BY_BOTH=${INVALID_ACCEPTED_BOTH}
RESULTS_FILE=${RESULTS_FILE}
FIXTURE_RESULTS_FILE=${FIXTURE_RESULTS_FILE}
WORKDIR=${WORKDIR}
SUMMARY

if [[ "${FAILURES}" -ne 0 ]]; then
    echo "FAIL: acceptance parity gate failed (${FAILURES} condition(s))" >&2
    exit 1
fi

log "PASS: acceptance parity gate"
exit 0
