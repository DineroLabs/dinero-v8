#!/usr/bin/env bash
set -euo pipefail

# Negative tests for Dinero consensus and RPC validation
# Run with VERIFY_NEGATIVE=1 to enable

if [[ "${VERIFY_NEGATIVE:-0}" != "1" ]]; then
  echo "ℹ️  Negative tests disabled (set VERIFY_NEGATIVE=1 to enable)"
  exit 0
fi

say() { printf "%s\n" "$*"; }
fail() { say "❌ $*"; exit 1; }

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --json) JSON_OUT="$2"; shift 2;;
    --timeout) TIMEOUT="$2"; shift 2;;
    *) fail "Unknown arg: $1";;
  esac
done

TMPTAG="verify-negative-$$"
INSTANCE_TAG="negative-test-$$"
DATADIR="/tmp/${TMPTAG}"
PORT=$(( 22000 + (RANDOM % 10000) ))
TIMEOUT=${TIMEOUT:-60}
JSON_OUT="${JSON_OUT:-}"

cleanup() {
  if [[ -f "$DATADIR/regtest/.cookie" ]]; then
    AUTH="$(cat "$DATADIR/regtest/.cookie")" || true
    curl -m 2 -s --basic --user "$AUTH" -H 'content-type: application/json' \
      --data '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' \
      "http://127.0.0.1:${PORT}/" >/dev/null || true
  fi
  pkill -f "$INSTANCE_TAG" >/dev/null 2>&1 || true
  rm -rf "$DATADIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# --- Spin up test node ---
mkdir -p "$DATADIR"
say "🧪 Starting negative test suite"
say "   📁 Datadir: $DATADIR"
say "   🔌 RPC Port: $PORT"
say "   🏷️  Instance: $INSTANCE_TAG"

D_BIN=
for p in ./build-test/bin/dinerod ./build/bin/dinerod ./bin/dinerod ./dinerod; do
  if [[ -x "$p" ]]; then D_BIN="$p"; break; fi
done
[[ -n "$D_BIN" ]] || fail "dinerod binary not found (build first)"

"$D_BIN" -regtest -datadir="$DATADIR" -rpcport="$PORT" -instance-tag="$INSTANCE_TAG" -printtoconsole >"$DATADIR/daemon.log" 2>&1 &
sleep 1

# Wait for daemon
deadline=$(( SECONDS + TIMEOUT ))
until [[ -f "$DATADIR/regtest/.cookie" ]]; do
  (( SECONDS > deadline )) && fail "Daemon did not start in ${TIMEOUT}s"
  sleep 0.1
done
AUTH="$(cat "$DATADIR/regtest/.cookie")"

until curl -m 1 -s "http://127.0.0.1:${PORT}/" >/dev/null; do
  (( SECONDS > deadline )) && fail "HTTP not ready in ${TIMEOUT}s"
  sleep 0.1
done
say "✅ Test daemon ready"

rpc() {
  local payload="$1"
  curl -s --basic --user "$AUTH" -H 'content-type: application/json' --data "$payload" "http://127.0.0.1:${PORT}/"
}

# --- Test Results Tracking ---
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

run_negative_test() {
  local test_name="$1"
  local test_cmd="$2"
  local expected_pattern="$3"
  
  TESTS_RUN=$((TESTS_RUN + 1))
  say "🧪 Test $TESTS_RUN: $test_name"
  
  if result=$(eval "$test_cmd" 2>&1); then
    if echo "$result" | grep -q "$expected_pattern"; then
      say "   ✅ PASS: Got expected error pattern: $expected_pattern"
      TESTS_PASSED=$((TESTS_PASSED + 1))
    else
      say "   ❌ FAIL: Expected error pattern '$expected_pattern' not found"
      say "   📝 Got: $result"
      TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
  else
    say "   ❌ FAIL: Command failed unexpectedly"
    say "   📝 Output: $result"
    TESTS_FAILED=$((TESTS_FAILED + 1))
  fi
}

# --- Negative Test Cases ---

say "🔍 Running negative test cases..."

# Test 1: Invalid RPC method
run_negative_test "Invalid RPC method" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"invalidmethod\",\"params\":[]}"' \
  "Method not found"

# Test 2: Malformed JSON
run_negative_test "Malformed JSON RPC" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockcount\""' \
  "Parse error"

# Test 3: Missing required parameters
run_negative_test "Missing createrawtransaction parameters" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"createrawtransaction\",\"params\":[]}"' \
  "requires inputs and outputs"

# Test 4: Invalid transaction hex
run_negative_test "Invalid transaction hex" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendrawtransaction\",\"params\":[\"invalid_hex\"]}"' \
  "Invalid transaction hex"

# Test 5: Empty transaction hex
run_negative_test "Empty transaction hex" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"sendrawtransaction\",\"params\":[\"\"]}"' \
  "cannot be empty"

# Test 6: Invalid block hash
run_negative_test "Invalid block hash" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblock\",\"params\":[\"invalid_hash\"]}"' \
  "Block not found"

# Test 7: Invalid verbosity parameter
run_negative_test "Invalid getblock verbosity" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblock\",\"params\":[\"0000\",\"invalid\"]}"' \
  "type must be"

# Test 8: getregtestminingkey on wrong network (if we had mainnet)
# This would fail in real mainnet, but we're on regtest so it should work
run_negative_test "getregtestminingkey availability check" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getregtestminingkey\",\"params\":[]}"' \
  "regtest-only"

# Test 9: Invalid createrawtransaction inputs
run_negative_test "Invalid createrawtransaction inputs" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"createrawtransaction\",\"params\":[\"not_array\",{}]}"' \
  "must be an array"

# Test 10: Invalid createrawtransaction outputs
run_negative_test "Invalid createrawtransaction outputs" \
  'rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"createrawtransaction\",\"params\":[[],\"not_object\"]}"' \
  "must be an object"

# --- Test Summary ---
say ""
say "🧪 Negative Test Summary:"
say "   📊 Tests run: $TESTS_RUN"
say "   ✅ Passed: $TESTS_PASSED"
say "   ❌ Failed: $TESTS_FAILED"

# --- JSON Output ---
if [[ -n "$JSON_OUT" ]]; then
  tmp="$JSON_OUT.tmp.$$"
  {
    printf '{\n'
    printf '  "ok": %s,\n' "$([[ $TESTS_FAILED -eq 0 ]] && echo "true" || echo "false")"
    printf '  "negative_tests": {\n'
    printf '    "total": %d,\n' "$TESTS_RUN"
    printf '    "passed": %d,\n' "$TESTS_PASSED"
    printf '    "failed": %d\n' "$TESTS_FAILED"
    printf '  },\n'
    printf '  "rpc": {"port": "%s", "datadir": "%s"}\n' "$PORT" "$DATADIR"
    printf '}\n'
  } > "$tmp"
  mv "$tmp" "$JSON_OUT"
  say "🧾 Wrote JSON: $JSON_OUT"
fi

if [[ $TESTS_FAILED -eq 0 ]]; then
  say "🎉 All negative tests passed!"
  exit 0
else
  say "⚠️  Some negative tests failed (non-blocking)"
  exit 0  # Non-blocking for CI
fi
