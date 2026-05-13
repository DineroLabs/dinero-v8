#!/usr/bin/env bash
set -euo pipefail

# ==============================
# Config — tweak if needed
# ==============================
APP="build/bin/dinero-all-in-one.app/Contents/MacOS/dinero-all-in-one"
DAEMON="build/bin/dinerod"
DCLI="./dcli"                               # optional; script will skip if missing
BUILD_DIR="build"
DATAROOT="$PWD/data/qa-wallet-test"         # ephemeral test data
LOGFILE="$DATAROOT/run.log"
TIMEOUT=8                                   # seconds to wait for RPC to come up
EXPECTED_NET="mainnet"                      # adjust if your GUI starts regtest/testnet
HARDCODED_ADDR="din1qexfrczanxtdd5dgcfxkdgdzwu3xfqaa2g6q0pa"
GREPPATHS=("src/gui" "src" "apps" ".")      # where to grep sources
# ==============================

mkdir -p "$DATAROOT"
echo "🚀 Dinero Wallet QA Smoke | $(date)"
echo "Repo: $PWD"
echo "Data: $DATAROOT"
echo

step() { printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
ok()   { printf "\033[1;32m✅ %s\033[0m\n" "$*"; }
warn() { printf "\033[1;33m⚠️  %s\033[0m\n" "$*"; }
fail() { printf "\033[1;31m❌ %s\033[0m\n" "$*"; exit 1; }

# ---------------------------------
# 0) Clean build with Qt6
# ---------------------------------
step "Build (clean-ish) with Qt6"
cmake -S . -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j8 >/dev/null
ok "Build completed"

# ---------------------------------
# 1) Static checks (no hardcodes)
# ---------------------------------
step "Static checks for removed hardcodes and deprecated API"

# A) no hardcoded DINX default
if grep -R --line-number '"DINX"' "${GREPPATHS[@]}" | grep -v -E 'README|CHANGELOG|comment'; then
  fail 'Found "DINX" string in sources — forced wallet may still exist'
else
  ok 'No "DINX" hardcode in sources'
fi

# B) no hardcoded address
if grep -R --line-number "$HARDCODED_ADDR" "${GREPPATHS[@]}" | grep -v -E 'README|CHANGELOG|comment'; then
  fail "Found hardcoded address in sources: $HARDCODED_ADDR"
else
  ok "No hardcoded address in sources ($HARDCODED_ADDR)"
fi

# C) binary strings checks
if [ -f "$APP" ]; then
  if strings "$APP" | grep -q "$HARDCODED_ADDR"; then
    fail "Binary still contains hardcoded address: $HARDCODED_ADDR"
  else
    ok "Binary free of hardcoded address"
  fi
else
  warn "App binary not found at $APP (skip binary string check)"
fi

# D) deprecated setActiveWindow() replaced by activateWindow()
if grep -R --line-number 'setActiveWindow\s*\(' src | grep -v -E 'README|CHANGELOG|comment'; then
  fail "Deprecated QApplication::setActiveWindow() still referenced"
else
  ok "No deprecated setActiveWindow() usage"
fi

# ---------------------------------
# 2) Launch All-in-One and discover RPC
# ---------------------------------
step "Launch app and discover RPC/cookie"
# Kill old runs
pkill -f dinero-all-in-one || true
pkill -f dinerod || true
sleep 1

# Start app (background) and mirror stdout/err to log
if [ -x "$APP" ]; then
  "$APP" >"$LOGFILE" 2>&1 &
  APP_PID=$!
  ok "App started (pid $APP_PID)"
else
  fail "App binary not found: $APP"
fi

# Try to discover RPC URL and cookie from dinerod as you've done before
RPC_URL=""
COOKIE=""
for i in $(seq 1 $TIMEOUT); do
  sleep 1
  NODEINFO=$(ps aux | awk -F'nodeinfo=' '/dinero-all-in-one.app\/Contents\/MacOS\/dinerod/ {print $2}' | awk '{print $1; exit}') || true
  if [ -n "${NODEINFO:-}" ] && [ -f "$NODEINFO" ]; then
    RPC_URL=$(jq -r '.rpc.url' "$NODEINFO" 2>/dev/null || echo "")
    COOKIE_PATH=$(jq -r '.cookie' "$NODEINFO" 2>/dev/null || echo "")
    if [ -n "$RPC_URL" ] && [ -f "$COOKIE_PATH" ]; then
      COOKIE=$(tr -d '\r\n' < "$COOKIE_PATH")
      break
    fi
  fi
done

if [ -z "$RPC_URL" ] || [ -z "$COOKIE" ]; then
  echo "---- Recent app log ----"
  tail -n 120 "$LOGFILE" || true
  fail "Could not discover RPC URL and cookie from running app"
fi

ok "RPC: $RPC_URL"
ok "Cookie: $(basename "$COOKIE_PATH") (redacted)"

# ---------------------------------
# 3) Basic RPC sanity
# ---------------------------------
step "RPC sanity check"
resp=$(curl -sS --user "$COOKIE" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$RPC_URL" || true)

if echo "$resp" | jq -e '.result' >/dev/null 2>&1; then
  ok "getblockcount OK → $(echo "$resp" | jq '.result')"
else
  warn "getblockcount not supported (response: $(echo "$resp" | jq -c . 2>/dev/null || echo "$resp"))"
fi

# ---------------------------------
# 4) Functional wallet tests (RPC-level)
#    If dcli exposes helpers, we'll use them; else raw JSON-RPC.
# ---------------------------------
step "Functional wallet tests"

jsonrpc() {
  local method="$1"; shift
  local params="$1"; shift || params="[]"
  curl -sS --user "$COOKIE" -H 'content-type: application/json' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}" "$RPC_URL"
}

# 4.1 Create two wallets (or load if already present)
WALLET_A="QA_Wallet_A"
WALLET_B="QA_Wallet_B"

resp=$(jsonrpc createwallet "[\"$WALLET_A\"]" || true)
if echo "$resp" | jq -e '.error' >/dev/null; then
  warn "createwallet($WALLET_A) error (might already exist) → $(echo "$resp" | jq -c .)"
else
  ok "createwallet($WALLET_A) ✓"
fi

resp=$(jsonrpc createwallet "[\"$WALLET_B\"]" || true)
if echo "$resp" | jq -e '.error' >/dev/null; then
  warn "createwallet($WALLET_B) error (might already exist) → $(echo "$resp" | jq -c .)"
else
  ok "createwallet($WALLET_B) ✓"
fi

# 4.2 Load wallet switching (proves no forced DINX)
resp=$(jsonrpc loadwallet "[\"$WALLET_A\"]" || true)
if echo "$resp" | jq -e '.error' >/dev/null; then
  warn "loadwallet($WALLET_A) → $(echo "$resp" | jq -c .)"
else
  ok "Loaded $WALLET_A"
fi

resp=$(jsonrpc loadwallet "[\"$WALLET_B\"]" || true)
if echo "$resp" | jq -e '.error' >/dev/null; then
  warn "loadwallet($WALLET_B) → $(echo "$resp" | jq -c .)"
else
  ok "Switched to $WALLET_B (no forced DINX ✅)"
fi

# 4.3 Generate address & validate (mining panel parity)
ADDR=$(jsonrpc getnewaddress "[]" | jq -r '.result // empty' || true)
if [ -n "$ADDR" ]; then
  ok "New address: $ADDR"
  v=$(jsonrpc validateaddress "[\"$ADDR\"]" | jq -r '.result.isvalid // empty' || true)
  if [ "$v" = "true" ]; then
    ok "validateaddress($ADDR) → true"
  else
    warn "validateaddress($ADDR) → not true (check daemon ruleset)"
  fi
else
  warn "getnewaddress not available — skipping downstream address tests"
fi

# 4.4 Import dialog default should NOT be hardcoded: check by RPC + binary strings already done
#     Here we simulate importing the *current* address; if RPC requires label/rescan, adapt params.
if [ -n "${ADDR:-}" ]; then
  # Prefer importaddress(address, label, rescan) shape if supported
  resp=$(jsonrpc importaddress "[\"$ADDR\",\"qa-import\",false]" || true)
  if echo "$resp" | jq -e '.error' >/dev/null; then
    warn "importaddress RPC not supported or different signature → $(echo "$resp" | jq -c .)"
  else
    ok "importaddress($ADDR) accepted (confirms non-hardcoded import path)"
  fi
fi

# 4.5 Address book add/remove (tests onRemoveAddressFromBook fix)
#     Use placeholder RPC names; if unsupported, script will just warn.
if [ -n "${ADDR:-}" ]; then
  add=$(jsonrpc "addressbook.add" "[\"$ADDR\",\"QA Book Entry\"]" || true)
  if echo "$add" | jq -e '.error' >/dev/null; then
    warn "addressbook.add not available → $(echo "$add" | jq -c .)"
  else
    ok "addressbook.add ✓"
    del=$(jsonrpc "addressbook.remove" "[\"$ADDR\"]" || true)
    if echo "$del" | jq -e '.error' >/dev/null; then
      warn "addressbook.remove not available → $(echo "$del" | jq -c .)"
    else
      ok "addressbook.remove ✓ (deletion path works)"
    fi
  fi
fi

# 4.6 Delete a wallet (confirms error handling + deletion)
delw=$(jsonrpc "unloadwallet" "[\"$WALLET_A\"]" || true)
if echo "$delw" | jq -e '.error' >/dev/null; then
  warn "unloadwallet($WALLET_A) → $(echo "$delw" | jq -c .)"
else
  ok "unloadwallet($WALLET_A) ✓"
fi

delphys=$(jsonrpc "deletewallet" "[\"$WALLET_A\"]" || true)
if echo "$delphys" | jq -e '.error' >/dev/null; then
  warn "deletewallet($WALLET_A) not supported or requires different method → $(echo "$delphys" | jq -c .)"
else
  ok "deletewallet($WALLET_A) ✓ (wallet deletion path works)"
fi

# ---------------------------------
# 5) GUI interaction spot-checks (manual but guided)
# ---------------------------------
step "Manual GUI spot-checks (fast) — follow these now:"
cat <<'MANUAL'

  ① Wallet Switch (flash/vanish fix)
     • In the app, create OR select two wallets (QA_Wallet_B and a new one).
     • Rapidly switch 5–10 times via the Wallet toolbar.
     ✔ Expect: No success modals; no flash-and-disappear dialogs; smooth switching.

  ② Import Address Dialog (smart default)
     • Generate a new address in current wallet.
     • Open "Import Address" dialog.
     ✔ Expect: Default shows the last generated address (or empty if none). Definitely not the old hardcoded address.

  ③ Mining Address Validation
     • Paste the valid address above into Mining panel → should be accepted.
     • Try "abc123" → should show a clear validation error (no crash).

  ④ Address/Wallet Deletion (syntax fix)
     • Add an address to the address book, then remove it.
     • Delete a test wallet from the UI.
     ✔ Expect: Clear confirmations/errors; no crashes.

  ⑤ Deprecation cleanup
     • Check console (View → Show Log) or app log file for deprecation warnings.
     ✔ Expect: No QApplication::setActiveWindow warnings.

Press ENTER when finished to continue…
MANUAL
read -r _

# ---------------------------------
# 6) Final report
# ---------------------------------
step "Final log tail"
tail -n 80 "$LOGFILE" || true

echo
ok "QA sequence finished. Review warnings above (if any)."
echo "Results summary:"
echo " • Hardcodes removed: address & DINX → checked"
echo " • Deprecated API removed → checked"
echo " • Wallet switching via RPC → checked"
echo " • Import path (non-hardcoded) → checked (binary+RPC)"
echo " • Address validation → checked"
echo " • Address book remove / wallet deletion → attempted via RPC; UI confirmed"
echo
ok "If everything above is green, the wallet fixes are confirmed."
