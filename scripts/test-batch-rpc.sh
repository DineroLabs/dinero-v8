#!/usr/bin/env bash
set -euo pipefail

# =========================
# Dinero - Batch RPC Test Suite (enhanced)
# =========================

# Config
RPC_URL="${DINERO_RPC:-http://127.0.0.1:20998}"
DATADIR="${DINERO_DATADIR:-${HOME}/.dinero}"
NETWORK="${DINERO_NETWORK:-mainnet}"

# Use environment variable if set, otherwise prefer current temp datadir if present
if [[ -n "${DINERO_DATADIR:-}" ]]; then
  DATADIR="$DINERO_DATADIR"
elif [[ -d "/tmp/test-dir2" && -f "/tmp/test-dir2/${NETWORK}/.cookie" ]]; then
  DATADIR="/tmp/test-dir2"
fi
COOKIE_FILE="${DATADIR}/${NETWORK}/.cookie"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

die() { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }

read_auth() {
  [[ -f "$COOKIE_FILE" ]] || die "Cookie file not found: $COOKIE_FILE
- Is dinerod running?
- Correct DATADIR/NETWORK? Current: DATADIR='$DATADIR' NETWORK='$NETWORK'
Tip: DINERO_DATADIR=/tmp/test-dir4 ./scripts/test-batch-rpc.sh
"
  cat "$COOKIE_FILE"
}

# --- Smart JSON param encoding (numbers/bools/null/JSON kept unquoted) ---
encode_param() {
  local v="${1:-}"
  # trim
  v="${v#"${v%%[![:space:]]*}"}"; v="${v%"${v##*[![:space:]]}"}"
  # raw JSON object/array
  if [[ "$v" =~ ^\{.*\}$ || "$v" =~ ^\[.*\]$ ]]; then printf '%s' "$v"; return; fi
  # bool/null (lowercase)
  case "${v,,}" in true|false|null) printf '%s' "${v,,}"; return ;; esac
  # integer
  if [[ "$v" =~ ^-?[0-9]+$ ]]; then printf '%s' "$v"; return; fi
  # string with escaping
  local esc="${v//\\/\\\\}"; esc="${esc//\"/\\\"}"; printf '"%s"' "$esc"
}

rpc() {
  local method="$1"; shift || true
  local params_json="[]"
  if [[ "$#" -gt 0 ]]; then
    local parts=()
    for x in "$@"; do parts+=("$(encode_param "$x")"); done
    params_json="[$(IFS=,; echo "${parts[*]}")]"
  fi
  local AUTH; AUTH="$(read_auth)"
  curl -s --user "$AUTH" \
    -H 'content-type: application/json' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
    "$RPC_URL"
}

rpc_raw() {
  local payload="$1"
  local AUTH; AUTH="$(read_auth)"
  curl -s --user "$AUTH" -H 'content-type: application/json' --data "$payload" "$RPC_URL"
}

assert_jsonrpc_success_item() {
  local js="$1"
  # has jsonrpc & id; result present and no error
  echo "$js" | jq -e 'has("jsonrpc") and has("id") and has("result") and (has("error")|not)' >/dev/null
}

assert_jsonrpc_error_item() {
  local js="$1"
  echo "$js" | jq -e 'has("jsonrpc") and has("id") and has("error") and (has("result")|not)' >/dev/null
}

# ---------------- Tests ----------------

# A) Test custom "batch" method (your server-side helper) if implemented
test_custom_batch_method() {
  echo -e "${BLUE}Testing custom \"batch\" method wrapper...${NC}"

  local batch_payload='{
    "jsonrpc": "2.0",
    "id": "batch_test",
    "method": "batch",
    "params": [
      { "jsonrpc": "2.0", "id": 1, "method": "getblockcount",     "params": [] },
      { "jsonrpc": "2.0", "id": 2, "method": "getblockchaininfo", "params": [] },
      { "jsonrpc": "2.0", "id": 3, "method": "getmininginfo",     "params": [] },
      { "jsonrpc": "2.0", "id": 4, "method": "getnetworkstats",   "params": [] }
    ]
  }'

  echo "Sending custom-batch request..."
  local response; response="$(rpc_raw "$batch_payload")"
  echo "Response received:"; echo "$response" | jq -C .

  # Some servers reply with an array (preferred), others with an object carrying 'result: []'
  if echo "$response" | jq -e 'type=="array"' >/dev/null 2>&1; then
    echo -e "${GREEN}✅ Custom batch returned an array${NC}"
    local count; count="$(echo "$response" | jq 'length')"
    [[ "$count" -eq 4 ]] || { echo -e "${RED}❌ Expected 4 items, got $count${NC}"; return 1; }

    # id mapping & item validations
    for id in 1 2 3 4; do
      local item; item="$(echo "$response" | jq ".[] | select(.id==$id)")"
      [[ -n "$item" ]] || { echo -e "${RED}❌ Missing response for id $id${NC}"; return 1; }
      if echo "$item" | jq -e 'has("result")' >/dev/null; then
        assert_jsonrpc_success_item "$item" || { echo -e "${RED}❌ Item id $id invalid success shape${NC}"; return 1; }
      elif echo "$item" | jq -e 'has("error")' >/dev/null; then
        assert_jsonrpc_error_item "$item"   || { echo -e "${RED}❌ Item id $id invalid error shape${NC}"; return 1; }
      else
        echo -e "${RED}❌ Item id $id has neither result nor error${NC}"; return 1;
      fi
    done
  else
    # Try object shape: { jsonrpc, id, result: [ ... ] }
    if echo "$response" | jq -e 'has("result") and (.result|type=="array")' >/dev/null 2>&1; then
      echo -e "${YELLOW}ℹ️ Custom batch returned an object with result[]; proceeding${NC}"
    else
      echo -e "${RED}❌ Unexpected custom-batch response shape${NC}"
      return 1
    fi
  fi

  echo -e "${GREEN}🎉 Custom \"batch\" test passed${NC}"
}

# B) Test **standard JSON-RPC batch** (top-level array)
test_standard_batch_array() {
  echo -e "${BLUE}Testing JSON-RPC standard batch (top-level array)...${NC}"

  # Build array with mixed methods
  local batch_array
  batch_array='[
    { "jsonrpc":"2.0","id": 10,"method":"getblockcount","params":[] },
    { "jsonrpc":"2.0","id": 11,"method":"getblockchaininfo","params":[] },
    { "jsonrpc":"2.0","id": 12,"method":"getmininginfo","params":[] }
  ]'

  local response; response="$(rpc_raw "$batch_array")"
  echo "Response:"; echo "$response" | jq -C .

  # Must be an array; order MAY differ; validate by id
  echo "$response" | jq -e 'type=="array" and (length==3)' >/dev/null \
    || { echo -e "${RED}❌ Standard batch did not return 3 items array${NC}"; return 1; }

  for id in 10 11 12; do
    local item; item="$(echo "$response" | jq ".[] | select(.id==$id)")"
    [[ -n "$item" ]] || { echo -e "${RED}❌ Missing response for id $id${NC}"; return 1; }
    # No "error": null; either result or error present
    if echo "$item" | jq -e 'has("result")' >/dev/null; then
      assert_jsonrpc_success_item "$item" || { echo -e "${RED}❌ Item id $id invalid success shape${NC}"; return 1; }
    elif echo "$item" | jq -e 'has("error")' >/dev/null; then
      assert_jsonrpc_error_item "$item"   || { echo -e "${RED}❌ Item id $id invalid error shape${NC}"; return 1; }
    else
      echo -e "${RED}❌ Item id $id has neither result nor error${NC}"; return 1;
    fi
  done

  echo -e "${GREEN}🎉 Standard batch array test passed${NC}"
}

# C) Mixed success/error batch (standard array)
test_standard_batch_mixed() {
  echo -e "${BLUE}Testing standard batch with mixed success/error...${NC}"

  # Include invalid method and bad param shape
  local body='[
    { "jsonrpc":"2.0","id":21,"method":"getblockcount","params":[] },
    { "jsonrpc":"2.0","id":22,"method":"nonexistentmethod","params":[] },
    { "jsonrpc":"2.0","id":23,"method":"getblockchaininfo","params":[] },
    { "jsonrpc":"2.0","id":24,"method":"invalidparams","params":"not_an_array" }
  ]'
  local response; response="$(rpc_raw "$body")"
  echo "Response:"; echo "$response" | jq -C .

  local success_count; success_count="$(echo "$response" | jq '[.[] | select(has("result"))] | length')"
  local error_count;   error_count="$(echo "$response" | jq '[.[] | select(has("error"))] | length')"
  echo "Success: $success_count  Error: $error_count"

  if (( success_count > 0 && error_count > 0 )); then
    echo -e "${GREEN}✅ Mixed batch handled both success and error${NC}"
  else
    echo -e "${RED}❌ Mixed batch did not include both success and error${NC}"
    return 1
  fi

  # Ensure each item has either result or error, not both, and never "error": null
  echo "$response" | jq -e '[ .[] | ( (has("result") and (has("error")|not)) or ((has("result")|not) and has("error")) ) ] | all' >/dev/null \
    || { echo -e "${RED}❌ Some items have invalid shape (both/neither result/error)${NC}"; return 1; }

  echo -e "${GREEN}🎉 Mixed standard batch test passed${NC}"
}

# D) Batch error handling (empty, oversized, invalid format)
test_batch_errors() {
  echo -e "${BLUE}Testing batch error handling...${NC}"

  # Empty standard batch (should be rejected per server policy — JSON-RPC allows empty array as no-op,
  # but most servers reject; our spec requires reject)
  echo "Empty batch request..."
  local empty='[]'
  local response; response="$(rpc_raw "$empty" || true)"
  # Expect error object for empty batch
  if echo "$response" | jq -e 'has("error")' >/dev/null 2>&1; then
    echo -e "${GREEN}✅ Empty batch rejected with error (spec-compliant)${NC}"
  else
    echo -e "${RED}❌ Empty batch should be rejected with error${NC}"
    return 1
  fi

  # Oversized batch limit test (101 items)
  echo "Oversized batch request..."
  local items=()
  for i in $(seq 1 101); do
    items+=( "{ \"jsonrpc\":\"2.0\",\"id\":$i,\"method\":\"getblockcount\",\"params\":[] }" )
  done
  local oversized='[ '"$(IFS=,; echo "${items[*]}")"' ]'
  response="$(rpc_raw "$oversized" || true)"
  if echo "$response" | jq -e 'has("error")' >/dev/null 2>&1; then
    echo -e "${GREEN}✅ Oversized batch rejected with error${NC}"
  else
    echo -e "${RED}❌ Oversized batch should be rejected by policy${NC}"
    return 1
  fi

  # Invalid format (params must be array/object/null; send string)
  echo "Invalid item format..."
  local invalid='[
    { "jsonrpc":"2.0","id":301,"method":"getblockhash","params":"not_an_array" }
  ]'
  response="$(rpc_raw "$invalid" || true)"
  if echo "$response" | jq -e '(type=="array") and (.[0] | has("error"))' >/dev/null 2>&1; then
    echo -e "${GREEN}✅ Invalid item rejected with error${NC}"
    # Check error code is -32602 (Invalid params)
    if echo "$response" | jq -e '(.[0] | .error.code) == -32602' >/dev/null 2>&1; then
      echo -e "${GREEN}✅ Correct error code (-32602) for invalid params${NC}"
    else
      echo -e "${YELLOW}ℹ️ Error code not -32602 (got: $(echo "$response" | jq -r '.[0].error.code'))${NC}"
    fi
  else
    echo -e "${RED}❌ Invalid item was not rejected${NC}"
    return 1
  fi

  # Test that simple methods now properly reject invalid params
  echo "Testing simple method parameter validation..."
  local simple_invalid='[
    { "jsonrpc":"2.0","id":302,"method":"getblockcount","params":"not_an_array" }
  ]'
  response="$(rpc_raw "$simple_invalid" || true)"
  if echo "$response" | jq -e '(type=="array") and (.[0] | has("error"))' >/dev/null 2>&1; then
    echo -e "${GREEN}✅ Simple method now rejects invalid params${NC}"
    # Check error code is -32602 (Invalid params)
    if echo "$response" | jq -e '(.[0] | .error.code) == -32602' >/dev/null 2>&1; then
      echo -e "${GREEN}✅ Correct error code (-32602) for simple method invalid params${NC}"
    else
      echo -e "${YELLOW}ℹ️ Error code not -32602 (got: $(echo "$response" | jq -r '.[0].error.code'))${NC}"
    fi
  else
    echo -e "${RED}❌ Simple method should reject invalid params${NC}"
    return 1
  fi

  echo -e "${GREEN}🎉 Batch error handling tests passed${NC}"
}

# E) Batch performance (quick signal)
test_batch_performance() {
  echo -e "${BLUE}Testing batch performance (10 getblockcount)...${NC}"

  # Measure single call avg over 5
  local t_sum=0
  for _ in {1..5}; do
    local t0 t1
    t0=$(date +%s%N)
    rpc getblockcount >/dev/null
    t1=$(date +%s%N)
    t_sum=$(( t_sum + ( (t1 - t0)/1000000 ) ))
  done
  local indiv_avg=$(( t_sum / 5 ))
  echo "Individual request avg (5): ${indiv_avg}ms"

  # Batch of 10 getblockcount
  local parts=()
  for i in $(seq 1 10); do
    parts+=( "{ \"jsonrpc\":\"2.0\",\"id\":$i,\"method\":\"getblockcount\",\"params\":[] }" )
  done
  local batch='[ '"$(IFS=,; echo "${parts[*]}")"' ]'

  local t0 t1
  t0=$(date +%s%N)
  rpc_raw "$batch" >/dev/null
  t1=$(date +%s%N)
  local batch_ms=$(( (t1 - t0)/1000000 ))
  local expected=$(( indiv_avg * 10 ))
  local efficiency=$(( expected * 100 / (batch_ms==0?1:batch_ms) ))

  echo "Batch (10 calls): ${batch_ms}ms"
  echo "Expected individual total: ${expected}ms"
  echo "Batch efficiency: ${efficiency}%"
  if (( batch_ms < expected )); then
    echo -e "${GREEN}✅ Batch more efficient than sequential${NC}"
  else
    echo -e "${YELLOW}ℹ️ Batch not faster here (small payloads/localhost)${NC}"
  fi

  # Header check on batch
  local AUTH; AUTH="$(read_auth)"
  if curl -sD - -o /dev/null --user "$AUTH" -H 'content-type: application/json' --data "$batch" "$RPC_URL" \
     | tr -d '\r' | grep -iq '^x-dinero-rpc-engine: v2'; then
    echo -e "${GREEN}✅ Header present on batch: x-dinero-rpc-engine: v2${NC}"
  else
    echo -e "${RED}❌ Header missing on batch response${NC}"
    return 1
  fi

  echo -e "${GREEN}🎉 Batch performance test completed${NC}"
}

# ---------------- Main ----------------
main() {
  echo -e "${GREEN}🧪 Testing Dinero Batch RPC Functionality${NC}"
  echo "=========================================="
  echo "RPC URL:  $RPC_URL"
  echo "DATADIR:  $DATADIR"
  echo "NETWORK:  $NETWORK"
  echo "=========================================="
  echo

  command -v jq >/dev/null || die "jq is required"
  command -v curl >/dev/null || die "curl is required"

  # Standard JSON-RPC batch (top-level array)
  test_standard_batch_array
  echo

  # Mixed success/error
  test_standard_batch_mixed
  echo

  # Error handling (empty, oversized, invalid)
  test_batch_errors
  echo

  # Custom "batch" method removed - use standard JSON-RPC 2.0 top-level array instead
  echo -e "${YELLOW}ℹ️ Custom 'batch' method removed - use standard top-level array batches${NC}"
  echo

  # Performance signal
  test_batch_performance
  echo

  echo -e "${GREEN}🎉 All batch RPC tests passed!${NC}"
  echo "Batch RPC functionality conforms and behaves correctly."
}

main "$@"
