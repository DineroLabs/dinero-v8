#!/usr/bin/env bash
#
# Regression repro for issue #309 — "miner that loses a tip block race gets
# stuck on its own minority fork; never reorgs to the higher-work chain."
#
# Unlike the single-node invalidateblock/reconsiderblock fork tests, this uses
# TWO real regtest nodes with a genuine network partition, so the loser must
# DISCOVER the winning branch over P2P and FETCH its bodies (the connect-reorg
# path). A harness with all bodies locally present skips the failing branch.
#
# Scenario:
#   common: A and B connected, mine to height H on the same chain.
#   partition (setnetworkactive false on both):
#     A (loser) mines its own block(s) -> A_tip @ H+LOSER (self-mined sibling)
#     B (winner) mines a heavier branch -> B_tip @ H+WINNER  (WINNER > LOSER)
#   heal (setnetworkactive true + addnode):
#     A learns B's heavier headers, must disconnect its own branch back to H,
#     fetch B's bodies, and connect them.
#
# PASS  = A reorgs to B_tip (height H+WINNER)         -> bug absent / fixed
# FAIL  = A stuck on its own tip (REORG ABORT loop)   -> bug #309 reproduced
#
# Tunables via env: LOSER (default 1), WINNER (default 4), BASE (default 6).
set -u

DINEROD="${DINEROD:-./build/dinerod}"
LOSER="${LOSER:-1}"      # blocks the loser mines on its own branch
WINNER="${WINNER:-4}"    # blocks the winner mines (must exceed LOSER for more work)
BASE="${BASE:-6}"        # common-history blocks before the fork
REORG_WAIT="${REORG_WAIT:-60}"   # seconds to allow the loser to reorg after heal

RED='\033[0;31m'; GREEN='\033[0;32m'; YEL='\033[1;33m'; BLU='\033[0;34m'; NC='\033[0m'

A_DIR="/tmp/dinero-wedge-A-$$"; B_DIR="/tmp/dinero-wedge-B-$$"
A_RPC=$((19200 + RANDOM % 400)); A_P2P=$((19600 + RANDOM % 400))
B_RPC=$((20200 + RANDOM % 400)); B_P2P=$((20600 + RANDOM % 400))
A_STRAT=$((21200 + RANDOM % 400)); B_STRAT=$((21600 + RANDOM % 400))
A_PID=""; B_PID=""

cleanup() {
    if [ "${KEEP_LOGS:-0}" = "1" ]; then
        cp -f "$A_DIR/node.log" /tmp/wedge-A.log 2>/dev/null || true
        cp -f "$B_DIR/node.log" /tmp/wedge-B.log 2>/dev/null || true
        echo -e "${YEL}Preserved logs: /tmp/wedge-A.log /tmp/wedge-B.log${NC}"
    fi
    if [ "${KEEP_DATADIR:-0}" = "1" ]; then
        # Preserve the loser datadir for a post-wedge reindex experiment.
        rm -rf /tmp/wedge-A-datadir 2>/dev/null || true
        cp -a "$A_DIR" /tmp/wedge-A-datadir 2>/dev/null || true
        echo -e "${YEL}Preserved loser datadir: /tmp/wedge-A-datadir (rpc was $A_RPC, p2p $A_P2P)${NC}"
    fi
    echo -e "${YEL}Cleaning up...${NC}"
    [ -n "$A_PID" ] && kill -TERM "$A_PID" 2>/dev/null || true
    [ -n "$B_PID" ] && kill -TERM "$B_PID" 2>/dev/null || true
    sleep 2
    pkill -f "dinerod.*dinero-wedge-[AB]-$$" 2>/dev/null || true
    rm -rf "$A_DIR" "$B_DIR"
}
trap cleanup EXIT

# rpc <dir> <rpcport> <method> [params...]
rpc() {
    local dir="$1" port="$2" method="$3"; shift 3
    local p="[" first=true
    for a in "$@"; do
        $first && first=false || p="$p,"
        if [ "$a" = "true" ] || [ "$a" = "false" ] || [[ "$a" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then p="$p$a"; else p="$p\"$a\""; fi
    done
    p="$p]"
    local cookie; cookie=$(cut -d: -f2 "$dir/.cookie" 2>/dev/null)
    curl -s --user "__cookie__:$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$p,\"id\":1}" \
        "http://127.0.0.1:$port" | jq -r '.result // .error // .'
}
rpcA() { rpc "$A_DIR" "$A_RPC" "$@"; }
rpcB() { rpc "$B_DIR" "$B_RPC" "$@"; }

start_node() { # <dir> <rpc> <p2p> <stratum> <logfile>  -> echoes PID
    mkdir -p "$1"
    "$DINEROD" --regtest --datadir="$1" --rpcport="$2" --port="$3" --stratumport="$4" --printtoconsole \
        > "$5" 2>&1 &
    echo $!
}
wait_rpc() { # <dir> <rpc>
    for _ in $(seq 1 40); do
        if [ "$(rpc "$1" "$2" blockchain.getblockcount 2>/dev/null)" != "" ]; then return 0; fi
        sleep 1
    done
    return 1
}

echo -e "${BLU}=== #309 repro: mined-loser reorg wedge (LOSER=$LOSER WINNER=$WINNER BASE=$BASE) ===${NC}"

echo -e "${BLU}[*]${NC} starting node A ($A_RPC) + node B ($B_RPC)"
A_LOG="$A_DIR/node.log"; B_LOG="$B_DIR/node.log"
A_PID=$(start_node "$A_DIR" "$A_RPC" "$A_P2P" "$A_STRAT" "$A_LOG")
B_PID=$(start_node "$B_DIR" "$B_RPC" "$B_P2P" "$B_STRAT" "$B_LOG")
wait_rpc "$A_DIR" "$A_RPC" || { echo -e "${RED}A failed to start${NC}"; exit 2; }
wait_rpc "$B_DIR" "$B_RPC" || { echo -e "${RED}B failed to start${NC}"; exit 2; }

ADDR_A=$(rpcA wallet.listaddresses | jq -r 'if type=="array" then .[0].address else empty end')
ADDR_B=$(rpcB wallet.listaddresses | jq -r 'if type=="array" then .[0].address else empty end')
echo -e "${YEL}[i]${NC} A addr=$ADDR_A  B addr=$ADDR_B"

# --- connect + common history ---
echo -e "${BLU}[*]${NC} connecting A<->B and mining $BASE common blocks on A"
rpcB addnode "127.0.0.1:$A_P2P" add >/dev/null
rpcA addnode "127.0.0.1:$B_P2P" add >/dev/null
sleep 3
rpcA mining.generatetoaddress "$BASE" "$ADDR_A" >/dev/null
# wait for B to sync to A
for _ in $(seq 1 30); do
    [ "$(rpcA blockchain.getblockcount)" = "$(rpcB blockchain.getblockcount)" ] && break; sleep 1
done
H=$(rpcA blockchain.getblockcount)
COMMON=$(rpcA blockchain.getbestblockhash)
echo -e "${YEL}[i]${NC} common height H=$H tip=$COMMON  (A=$(rpcA blockchain.getblockcount) B=$(rpcB blockchain.getblockcount))"

# --- partition ---
echo -e "${BLU}[*]${NC} partitioning (setnetworkactive false on both)"
rpcA setnetworkactive false >/dev/null
rpcB setnetworkactive false >/dev/null
sleep 2

# --- diverge: A mines its own (losing) branch, B mines the heavier branch ---
echo -e "${BLU}[*]${NC} A mines $LOSER (self-mined loser), B mines $WINNER (heavier)"
rpcA mining.generatetoaddress "$LOSER" "$ADDR_A" >/dev/null
rpcB mining.generatetoaddress "$WINNER" "$ADDR_B" >/dev/null
A_TIP=$(rpcA blockchain.getbestblockhash); A_H=$(rpcA blockchain.getblockcount)
B_TIP=$(rpcB blockchain.getbestblockhash); B_H=$(rpcB blockchain.getblockcount)
echo -e "${YEL}[i]${NC} A(loser) tip=$A_TIP @${A_H}   B(winner) tip=$B_TIP @${B_H}"
if [ "$B_H" -le "$A_H" ]; then
    echo -e "${RED}[setup error]${NC} winner not higher ($B_H <= $A_H); raise WINNER"; exit 2
fi

# --- heal partition ---
echo -e "${BLU}[*]${NC} healing partition; A must reorg from its own tip to B's heavier chain"
rpcA setnetworkactive true >/dev/null
rpcB setnetworkactive true >/dev/null
rpcA addnode "127.0.0.1:$B_P2P" onetry >/dev/null
rpcB addnode "127.0.0.1:$A_P2P" onetry >/dev/null

# --- observe whether A reorgs ---
for _ in $(seq 1 "$REORG_WAIT"); do
    cur=$(rpcA blockchain.getbestblockhash)
    [ "$cur" = "$B_TIP" ] && break
    sleep 1
done

A_FINAL_TIP=$(rpcA blockchain.getbestblockhash); A_FINAL_H=$(rpcA blockchain.getblockcount)
# DECISIVE PROBE (#309): for each winner-branch height, get B's canonical hash,
# then ask A if that body is present (getblock returns it if stored, errors if not).
echo -e "${YEL}[probe]${NC} winner-branch body presence on A (loser):"
for h in $(seq "$H" "$B_H"); do
    bh=$(rpcB blockchain.getblockhash "$h" 2>/dev/null)
    ablk=$(rpcA blockchain.getblock "$bh" 1 2>/dev/null)
    if echo "$ablk" | grep -qiE "\"height\"|\"tx\"|merkle"; then present="PRESENT"; else present="ABSENT"; fi
    echo -e "   h=$h Bhash=${bh:0:16}…  A-body=$present"
done
# Did A actually reconnect + learn B's heavier header chain? (distinguishes the
# connect-reorg wedge from a "never reconnected" test artifact)
A_PEERS=$(rpcA net.getconnectioncount 2>/dev/null); [ -z "$A_PEERS" ] && A_PEERS=$(rpcA getconnectioncount 2>/dev/null)
A_SAW_BETTER=$(grep -acE "HEADER CHAIN IS BETTER|REORG ABORT|ConnectTip FAILED" "$A_LOG" 2>/dev/null)
echo -e "${YEL}[i]${NC} after heal: A tip=$A_FINAL_TIP @${A_FINAL_H}  (want B tip=$B_TIP @${B_H})"
echo -e "${YEL}[i]${NC} A peers=$A_PEERS  A saw-better/abort log lines=$A_SAW_BETTER"

if [ "$A_FINAL_TIP" = "$B_TIP" ] && [ "$A_FINAL_H" = "$B_H" ]; then
    echo -e "${GREEN}=== PASS: loser reorged to the heavier chain (bug absent/fixed) ===${NC}"
    exit 0
else
    echo -e "${RED}=== FAIL: loser STUCK on its own minority fork (#309 reproduced) ===${NC}"
    echo -e "${RED}    A stayed at $A_FINAL_TIP @${A_FINAL_H}; expected $B_TIP @${B_H}${NC}"
    if [ "${A_PEERS:-0}" = "0" ] && [ "${A_SAW_BETTER:-0}" = "0" ]; then
        echo -e "${YEL}    NOTE: A has 0 peers and never saw a better chain — could be a reconnect artifact, not the wedge. Inspect log.${NC}"
    else
        echo -e "${GREEN}    CONFIRMED connect-reorg path: A reconnected ($A_PEERS peers) and/or saw the better chain but did not switch.${NC}"
    fi
    echo -e "${YEL}    A log (reorg/connect lines):${NC}"
    grep -aE "HEADER CHAIN IS BETTER|REORG ABORT|ConnectTip FAILED|HEIGHT ADJ|best_candidate == active_tip|Stored block accepted but not active|DROPPING unsolicited" "$A_LOG" 2>/dev/null | tail -20 || true
    exit 1
fi
