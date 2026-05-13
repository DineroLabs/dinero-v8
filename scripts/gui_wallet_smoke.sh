#!/usr/bin/env bash
set -euo pipefail

# ---------- CONFIG (override with env vars) ----------
APP_BIN="${APP_BIN:-build/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop}"
PASSPHRASE="${PASSPHRASE:-test-pass-123}"
UNLOCK_SECS="${UNLOCK_SECS:-600}"
REGTEST="${REGTEST:-1}"     # use regtest GUI/daemon
WIF_TEST="${WIF_TEST:-}"    # set to a valid WIF to test WIF import
VAULT_BLOB="${VAULT_BLOB:-}"        # base64/hex blob to test vault import
VAULT_PASSPHRASE="${VAULT_PASSPHRASE:-}" # passphrase for vault blob

# ---------- helpers ----------
log() { printf "\n\033[1m%s\033[0m\n" "$*"; }

curl_json() {
  local method="$1" ; shift
  local data="$1"   ; shift || true
  local auth_header=()
  if [[ -n "${BEARER_TOKEN:-}" ]]; then
    auth_header=(-H "Authorization: Bearer ${BEARER_TOKEN}")
  else
    auth_header=(-u "${COOKIE_VAL}")
  fi
  curl -sS --max-time 5 \
    -H 'content-type: application/json' \
    "${auth_header[@]}" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"${method}\",\"params\":${data:-[]}}" \
    "http://127.0.0.1:${RPC_PORT}/"
}

die() { echo "ERROR: $*" >&2; exit 1; }

detect_port() {
  # Prefer canonical regtest RPC, then an alternate local port, else sniff from lsof
  for p in 20996 40998; do
    if curl -sS --max-time 1 "http://127.0.0.1:$p/healthz" 2>/dev/null | grep -q '"status":"ok"'; then
      echo "$p"; return
    fi
  done
  # Last resort: sniff dinerod listening port
  local p
  p=$(lsof -nP -iTCP -sTCP:LISTEN 2>/dev/null | awk '/dinerod/ && /127\.0\.0\.1:/{print $9}' | sed 's/.*://;q') || true
  [[ -n "$p" ]] && echo "$p" && return
  echo ""
}

find_cookie() {
  if [[ -n "${DINERO_COOKIE_FILE:-}" ]]; then
    [[ -f "$DINERO_COOKIE_FILE" ]] && { echo "$DINERO_COOKIE_FILE"; return; }
  fi

  # Canonical locations
  local cands=(
    "$HOME/.dinero/regtest/.cookie"
    "$HOME/.dinero/.cookie"
  )
  for c in "${cands[@]}"; do
    [[ -f "$c" ]] && { echo "$c"; return; }
  done
  echo ""
}

mint_bearer() {
  local tok
  tok=$(curl -sS -u "${COOKIE_VAL}" -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":"x","method":"rpc.createauth"}' \
    "http://127.0.0.1:${RPC_PORT}/" 2>/dev/null | jq -r '.result.token // empty') || true
  [[ -n "$tok" ]] && echo "$tok" || echo ""
}

# ---------- 0) kill leftovers & launch GUI ----------
log "Killing leftovers"
pkill -f dinerod  || true
pkill -f dinero-desktop || true

log "Launching GUI in background"
"${APP_BIN}" >/dev/null 2>&1 & GUI_PID=$!
disown "$GUI_PID"

# ---------- 1) wait for health & discover port ----------
log "Waiting for daemon health..."
for i in {1..60}; do
  RPC_PORT=$(detect_port || true)
  if [[ -n "${RPC_PORT:-}" ]]; then
    if curl -sS "http://127.0.0.1:${RPC_PORT}/healthz" 2>/dev/null | jq . >/dev/null 2>&1; then
      break
    fi
  fi
  sleep 1
done
[[ -n "${RPC_PORT:-}" ]] || die "Daemon health not ready"

log "Daemon RPC port = $RPC_PORT"

# ---------- 2) acquire cookie & bearer ----------
COOKIE_PATH=$(find_cookie)
[[ -n "$COOKIE_PATH" ]] || die "No .cookie found"
COOKIE_VAL=$(cat "$COOKIE_PATH")
[[ "$COOKIE_VAL" =~ : ]] || die "Cookie malformed"

log "Minting Bearer token"
BEARER_TOKEN=$(mint_bearer || true)
if [[ -z "$BEARER_TOKEN" ]]; then
  log "Bearer mint failed; falling back to cookie auth"
else
  log "Bearer token acquired"
fi

# ---------- 3) sanity: network + wallet ----------
log "getnetworkinfo"
curl_json getnetworkinfo '[]' | jq .

log "getwalletinfo"
WI=$(curl_json getwalletinfo '[]')
echo "$WI" | jq .

ENCRYPTED=$(echo "$WI" | jq -r '.result.encrypted // false')
UNLOCKED_UNTIL=$(echo "$WI" | jq -r '.result.unlocked_until // 0')

# ---------- 4) encrypt if needed, then unlock ----------
if [[ "$ENCRYPTED" != "true" ]]; then
  log "Encrypting wallet"
  curl_json wallet.encrypt "{\"passphrase\":\"$PASSPHRASE\"}" | jq .
  # Some wallets require a restart; try unlock anyway
  sleep 1
fi

if [[ "${UNLOCKED_UNTIL:-0}" -le 0 ]]; then
  log "Unlocking wallet for ${UNLOCK_SECS}s"
  curl_json wallet.unlock "{\"passphrase\":\"$PASSPHRASE\",\"timeout\":$UNLOCK_SECS}" | jq .
fi

# ---------- 5) address + balance ----------
log "New address"
ADDR=$(curl_json getnewaddress '[]' | jq -r '.result')
echo "Address: $ADDR"

log "Validate address"
curl_json validateaddress "[\"$ADDR\"]" | jq .

log "Balance"
curl_json getbalance '[]' | jq .

# ---------- 6) optional: import WIF ----------
if [[ -n "${WIF_TEST}" ]]; then
  log "Importing WIF (with no rescan)"
  curl_json wallet.import "{\"type\":\"wif\",\"wif\":\"$WIF_TEST\",\"rescan\":false}" | jq .
else
  log "Skipping WIF import (set WIF_TEST to enable)"
fi

# ---------- 7) optional: import encrypted vault ----------
if [[ -n "${VAULT_BLOB}" && -n "${VAULT_PASSPHRASE}" ]]; then
  log "Importing encrypted vault (with no rescan)"
  curl_json wallet.vault.import "{\"blob\":\"$VAULT_BLOB\",\"passphrase\":\"$VAULT_PASSPHRASE\",\"rescan\":false}" | jq .
else
  log "Skipping vault import (set VAULT_BLOB and VAULT_PASSPHRASE to enable)"
fi

# ---------- 8) backup → restore round-trip (smoke) ----------
TMPDIR="$(mktemp -d)"
BACKUP="$TMPDIR/wallet-backup.dat"

log "Backing up wallet → $BACKUP"
curl_json wallet.backup "{\"path\":\"$BACKUP\"}" | jq .

log "Restoring backup (no overwrite)"
curl_json wallet.restore "{\"path\":\"$BACKUP\",\"overwrite\":false}" | jq .

# ---------- 9) start a short rescan from now ----------
NOW=$(date +%s)
log "Starting rescan from now=$NOW"
curl_json wallet.rescan "{\"from_time\":$NOW}" | jq .

log "List transactions"
curl_json listtransactions '[]' | jq .

log "✅ Smoke test complete."
