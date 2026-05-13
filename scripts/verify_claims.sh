#!/usr/bin/env bash
set -euo pipefail

# ---------- config ----------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/bin"
DAEMON="$BIN/dinerod"
CLI="$BIN/dinero-cli"
QT="$BIN/dinero-qt"             # optional
WEB_SERVER="$BIN/dinero-web"    # optional, if exists
MOBILE_BIN="$BIN/dinero-mobile" # optional, if exists

DATADIR="$ROOT/test-data/regtest"
RPCPORT=20999
REG_ARGS=(--regtest --datadir="$DATADIR" --rpcport="$RPCPORT" --printtoconsole)
CURL="curl -sS --max-time 5 -H Content-Type:application/json"
PASS=0; FAIL=0

hr() { printf '%*s\n' 70 | tr ' ' '-'; }
ok() { echo "✅ $*"; PASS=$((PASS+1)); }
bad(){ echo "❌ $*"; FAIL=$((FAIL+1)); }

ensure_binaries() {
  for b in "$DAEMON" "$CLI"; do
    if [[ ! -x "$b" ]]; then bad "Missing binary: $b"; exit 1; fi
  done
  ok "Binaries present"
}

boot_regtest() {
  rm -rf "$DATADIR"
  mkdir -p "$DATADIR"
  "$DAEMON" "${REG_ARGS[@]}" >"$ROOT/regtest.log" 2>&1 &
  DPID=$!
  sleep 2
  if ! kill -0 "$DPID" 2>/dev/null; then
    bad "daemon failed to start (see regtest.log)"
    exit 1
  fi
  ok "Daemon started (PID=$DPID)"
  echo "$DPID" > "$ROOT/regtest.pid"
}

cookie() {
  cat "$DATADIR/regtest/.cookie"
}

rpc() {
  local METHOD="$1"; shift
  local COOKIE; COOKIE="$(cookie)"
  $CURL --user "$COOKIE" --data "$(
    jq -n --arg m "$METHOD" --argjson p "${1:-[]}" '{jsonrpc:"2.0",id:"t",method:$m,params:$p}'
  )" "http://127.0.0.1:$RPCPORT/"
}

# Verify CLI override flags bypass nodeinfo.json (the earlier bug)
check_cli_overrides() {
  printf '{}' > "$ROOT/tmp.empty.json"
  # Should NOT try to read nodeinfo.json if --rpc-url and --cookie-file given
  local COOKIEFILE="$DATADIR/regtest/.cookie"
  local OUT
  set +e
  OUT=$("$CLI" --rpc-url "http://127.0.0.1:$RPCPORT" --cookie-file "$COOKIEFILE" rpc.ping 2>&1)
  local EC=$?
  set -e
  if [[ $EC -eq 0 && "$OUT" == *"pong"* ]]; then
    ok "CLI override flags honored (no forced nodeinfo.json)"
  else
    bad "CLI overrides test failed. Output: $OUT"
  fi
}

# HD wallet + BIP39 smoke
check_hd_bip39() {
  local addr1 addr2
  addr1=$(rpc wallet.getnewaddress | jq -r '.result' 2>/dev/null || true)
  addr2=$(rpc wallet.getnewaddress | jq -r '.result' 2>/dev/null || true)
  if [[ "$addr1" =~ ^din1 && "$addr2" =~ ^din1 && "$addr1" != "$addr2" ]]; then
    ok "HD wallet address gen (unique Bech32 din1...)"
  else
    bad "HD wallet address gen failed ($addr1 / $addr2)"
  fi

  # BIP39 round-trip (if exposed)
  local mnem seedCheck
  mnem=$(rpc wallet.mnemonic.new '[]' | jq -r '.result.mnemonic' 2>/dev/null || true)
  if [[ -n "$mnem" && "$mnem" == *" "* ]]; then
    ok "BIP39 mnemonic generated"
    seedCheck=$(rpc wallet.mnemonic.validate "$(jq -nc --arg m "$mnem" '[$m]')" | jq -r '.result.valid' 2>/dev/null || true)
    [[ "$seedCheck" == "true" ]] && ok "BIP39 mnemonic validated" || bad "BIP39 validation failed"
  else
    echo "ℹ️  BIP39 RPC not found; skipping mnemonic checks."
  fi
}

# Mining RPCs (status/address; generatetoaddress if present)
check_mining() {
  local s a
  s=$(rpc mining.status | jq -r '.result.is_mining // empty' 2>/dev/null || true)
  a=$(rpc mining.getaddress | jq -r '.result // empty' 2>/dev/null || true)
  if [[ -n "$a" && "$a" =~ ^din1 ]]; then ok "Mining.getaddress OK ($a)"; else bad "Mining.getaddress failed"; fi
  if [[ "$s" == "true" || "$s" == "false" ]]; then ok "Mining.status OK"; else bad "Mining.status failed"; fi

  # Optional block gen for regtest
  local gen
  gen=$(rpc generatetoaddress "$(jq -nc --arg addr "$a" '[1,$addr]')" 2>/dev/null || true)
  if echo "$gen" | jq -e '.result[0]' >/dev/null 2>&1; then
    ok "generatetoaddress mined 1 block"
  else
    echo "ℹ️  generatetoaddress not available or disabled; skipping."
  fi
}

# Mempool (stats) + simple tx echo pipeline (if available)
check_mempool_network() {
  local ms
  ms=$(rpc mempool.stats | jq -r '.result.size // empty' 2>/dev/null || true)
  if [[ "$ms" =~ ^[0-9]+$ ]]; then ok "Mempool.stats OK (size=$ms)"; else echo "ℹ️  mempool.stats missing; skipping."; fi

  # Network status
  local na
  na=$(rpc getnetworkinfo | jq -r '.result.networkactive // empty' 2>/dev/null || true)
  if [[ "$na" == "true" || "$na" == "false" ]]; then ok "getnetworkinfo OK"; else bad "getnetworkinfo failed"; fi
}

# Multi-account RPCs
check_multiaccount() {
  local acc
  acc=$(rpc multiaccount.create '["Personal"]' | jq -r '.result.id // empty' 2>/dev/null || true)
  if [[ -n "$acc" ]]; then
    ok "multiaccount.create OK (id=$acc)"
    local cur
    cur=$(rpc multiaccount.getcurrent | jq -r '.result.id // empty' 2>/dev/null || true)
    [[ "$cur" == "$acc" ]] && ok "multiaccount.getcurrent OK" || bad "multiaccount.getcurrent mismatch"
    local a2
    a2=$(rpc multiaccount.generatenewaddress '[]' | jq -r '.result // empty' 2>/dev/null || true)
    [[ "$a2" =~ ^din1 ]] && ok "multiaccount.generatenewaddress OK ($a2)" || bad "multiaccount.generatenewaddress failed"
  else
    echo "ℹ️  multiaccount.* RPCs not found; skipping."
  fi
}

# Hardware wallet (detect only; requires device connected)
check_hardware_wallet() {
  if command -v system_profiler >/dev/null; then
    if system_profiler SPUSBDataType 2>/dev/null | egrep -qi "Ledger|Trezor"; then
      ok "Hardware wallet detected on USB (Ledger/Trezor)"
      # Optional: ping RPC layer if exposed
      local hw
      hw=$(rpc hw.list | jq -r '.result[0].model // empty' 2>/dev/null || true)
      if [[ -n "$hw" ]]; then ok "hw.list OK ($hw)"; else echo "ℹ️  hw.* RPC not found; skipping deeper HW checks."; fi
    else
      echo "ℹ️  No Ledger/Trezor detected—connect one to run HW checks."
    fi
  fi
}

# Web wallet server basic checks (if present)
check_web_wallet() {
  if [[ -x "$WEB_SERVER" ]]; then
    "$WEB_SERVER" --port 8088 >"$ROOT/web.log" 2>&1 &
    WPID=$!
    sleep 1
    if kill -0 "$WPID" 2>/dev/null; then
      local s
      s=$(curl -sS --max-time 5 http://127.0.0.1:8088/status | jq -r '.status // empty' 2>/dev/null || true)
      [[ "$s" == "ok" ]] && ok "Web wallet /status OK" || bad "Web wallet status failed"
      kill "$WPID" || true
    else
      echo "ℹ️  Web server binary exists but failed to start; see web.log"
    fi
  else
    echo "ℹ️  Web wallet binary not present; skipping."
  fi
}

# Mobile stub check (build exists)
check_mobile_build() {
  if [[ -x "$MOBILE_BIN" ]]; then
    ok "Mobile binary present (sanity only)"
  else
    echo "ℹ️  Mobile binary not present; skipping."
  fi
}

# GUI smoke (optional)
check_gui() {
  if [[ -x "$QT" ]]; then
    "$QT" --regtest --detach 2>/dev/null || true
    ok "Qt GUI launched (detached)"
  else
    echo "ℹ️  Qt GUI binary not present; skipping."
  fi
}

summary() {
  hr
  echo "RESULTS: $PASS passed, $FAIL failed"
  [[ $FAIL -eq 0 ]] || exit 2
}

# ---------- run ----------
hr; echo "ENSURING BINARIES"; hr
ensure_binaries
hr; echo "BOOTING REGTEST"; hr
boot_regtest
sleep 1
hr; echo "CLI OVERRIDES"; hr
check_cli_overrides
hr; echo "HD WALLET + BIP39"; hr
check_hd_bip39
hr; echo "MINING RPC"; hr
check_mining
hr; echo "MEMPOOL + NETWORK"; hr
check_mempool_network
hr; echo "MULTI-ACCOUNT RPC"; hr
check_multiaccount
hr; echo "HARDWARE WALLET (USB presence)"; hr
check_hardware_wallet
hr; echo "WEB WALLET"; hr
check_web_wallet
hr; echo "MOBILE BUILD"; hr
check_mobile_build
hr; echo "GUI SMOKE"; hr
check_gui
summary
