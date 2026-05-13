#!/usr/bin/env bash
set -euo pipefail

# =========================
# Dinero RPC Performance Benchmark (enhanced)
# =========================
# Usage: ./scripts/rpc-benchmark.sh [iterations] [concurrent]
# Example: ./scripts/rpc-benchmark.sh 100 5
#
# Env overrides (reasonable localhost defaults):
#   DINERO_RPC=http://127.0.0.1:20998
#   DINERO_DATADIR=/tmp/test-dir2
#   DINERO_NETWORK=mainnet
#   SLO_P95_MS=25        # pass gate if p95 <= 25ms
#   SLO_P99_MS=40        # pass gate if p99 <= 40ms
#   SLO_ENFORCE=1        # set 0 to not exit non-zero on SLO miss
#   JSON_OUT=/tmp/rpc_benchmark_results.json

# -------- Config --------
ITERATIONS="${1:-100}"
CONCURRENT="${2:-5}"
RPC_URL="${DINERO_RPC:-http://127.0.0.1:20998}"
DATADIR="${DINERO_DATADIR:-${HOME}/.dinero}"
NETWORK="${DINERO_NETWORK:-mainnet}"

# Prefer your current temp datadir automatically if it exists
if [[ -d "/tmp/test-dir2" && -f "/tmp/test-dir2/${NETWORK}/.cookie" ]]; then
  DATADIR="/tmp/test-dir2"
fi

COOKIE_FILE="${DATADIR}/${NETWORK}/.cookie"

# SLOs (localhost, release build; tune as needed)
SLO_P95_MS="${SLO_P95_MS:-25}"
SLO_P99_MS="${SLO_P99_MS:-40}"
SLO_ENFORCE="${SLO_ENFORCE:-0}"
JSON_OUT="${JSON_OUT:-}"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

die() { echo -e "${RED}ERROR: $*${NC}" >&2; exit 1; }

# -------- Prereqs --------
command -v jq >/dev/null || die "jq is required but not installed"
command -v curl >/dev/null || die "curl is required but not installed"
command -v awk >/dev/null  || die "awk is required but not installed"

read_auth() {
  [[ -f "$COOKIE_FILE" ]] || die "Cookie not found: $COOKIE_FILE
- Is dinerod running?
- DATADIR/NETWORK correct? Current: DATADIR='$DATADIR' NETWORK='$NETWORK'
Tip: DINERO_DATADIR=/tmp/test-dir2 ./scripts/rpc-benchmark.sh
"
  cat "$COOKIE_FILE"
}

# --- Smart JSON param encoding (numbers/bools/null/JSON kept unquoted) ---
encode_param() {
  local v="$1"
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

# Percentiles + stats via awk.
percentiles() {
  awk '
  {
    n++; v[n]=$1; sum+=$1
  }
  END {
    if (n==0) { print "p50=0 p90=0 p95=0 p99=0 mean=0 sd=0"; exit }
    # sort
    asort(v)
    # helper
    function pct(p){ idx=int((p/100.0)*n+0.5); if(idx<1)idx=1; if(idx>n)idx=n; return v[idx] }
    mean=sum/n
    s2=0
    for(i=1;i<=n;i++){ d=v[i]-mean; s2+=d*d }
    sd=(n>1)?sqrt(s2/(n-1)):0
    printf "p50=%d p90=%d p95=%d p99=%d mean=%.1f sd=%.1f", pct(50), pct(90), pct(95), pct(99), mean, sd
  }'
}

emit_json_metric() {
  local method="$1" avg="$2" p95="$3" p99="$4" minv="$5" maxv="$6" success="$7" errors="$8"
  [[ -z "$JSON_OUT" ]] && return 0
  if [[ ! -f "$JSON_OUT" ]]; then echo "[]" > "$JSON_OUT"; fi
  tmp="$(mktemp)"
  jq --arg m "$method" \
     --argjson avg "$avg" --argjson p95 "$p95" --argjson p99 "$p99" \
     --argjson min "$minv" --argjson max "$maxv" \
     --argjson success "$success" --argjson errors "$errors" \
     '. + [{"method":$m,"avg_ms":$avg,"p95_ms":$p95,"p99_ms":$p99,"min_ms":$min,"max_ms":$max,"success":$success,"errors":$errors}]' \
     "$JSON_OUT" > "$tmp" && mv "$tmp" "$JSON_OUT"
}

gate_check() {
  local method="$1" p95="$2" p99="$3"
  local fail=0
  if (( p95 > SLO_P95_MS )); then
    echo -e "${YELLOW}⚠️  $method p95=${p95}ms > SLO ${SLO_P95_MS}ms${NC}"; fail=1
  fi
  if (( p99 > SLO_P99_MS )); then
    echo -e "${YELLOW}⚠️  $method p99=${p99}ms > SLO ${SLO_P99_MS}ms${NC}"; fail=1
  fi
  if (( fail==1 && SLO_ENFORCE==1 )); then
    echo -e "${RED}❌ SLO gate failed for $method${NC}"; exit 1
  fi
}

# Warm up a method (discard results)
warm_up() {
  local method="$1"; shift || true
  local -a PARAMS=( "$@" )
  for _ in {1..20}; do 
    if (( ${#PARAMS[@]} > 0 )); then
      rpc "$method" "${PARAMS[@]}" >/dev/null
    else
      rpc "$method" >/dev/null
    fi
  done
}

# Run a method many times, gather latency stats
bench_series() {
  local method="$1"; shift || true
  local -a PARAMS=( "$@" )

  echo -e "${BLUE}Benchmarking $method...${NC}"
  if (( ${#PARAMS[@]} > 0 )); then
    warm_up "$method" "${PARAMS[@]}"
  else
    warm_up "$method"
  fi

  local -a LATS=()
  local min_time=999999 max_time=0 success=0 errors=0

  for ((i=1;i<=ITERATIONS;i++)); do
    local t0 t1 dur
    t0=$(date +%s%N)
    local out
    if (( ${#PARAMS[@]} > 0 )); then
      out="$(rpc "$method" "${PARAMS[@]}")"
    else
      out="$(rpc "$method")"
    fi
    t1=$(date +%s%N)
    dur=$(( (t1 - t0) / 1000000 )) # ms

    if echo "$out" | jq -e 'has("error") | not' >/dev/null 2>&1; then
      ((success++))
      LATS+=( "$dur" )
      (( dur < min_time )) && min_time=$dur
      (( dur > max_time )) && max_time=$dur
    else
      ((errors++))
      [[ $((i%10)) -eq 0 ]] && echo -n "."
    fi
    [[ $((i%10)) -eq 0 ]] && echo -n "."
  done
  echo

  local stats avg p50 p90 p95 p99 sd
  if (( success > 0 )); then
    stats="$(printf "%s\n" "${LATS[@]}" | percentiles)"
    # parse stats line: p50=.. p90=.. p95=.. p99=.. mean=.. sd=..
    eval "$stats"   # sets p50 p90 p95 p99 mean sd
    avg=$(printf '%.0f' "$mean")
    echo -e "${GREEN}✅ $method Results:${NC}"
    echo "  Success: $success/$ITERATIONS"
    echo "  Avg: ${avg}ms | Min: ${min_time}ms | Max: ${max_time}ms"
    echo "  p50: ${p50}ms | p90: ${p90}ms | p95: ${p95}ms | p99: ${p99}ms | sd: ${sd}ms"
    emit_json_metric "$method" "$avg" "$p95" "$p99" "$min_time" "$max_time" "$success" "$errors"
    gate_check "$method" "$p95" "$p99"
  else
    echo -e "${RED}❌ $method failed all iterations${NC}"
    emit_json_metric "$method" 0 0 0 0 0 0 "$errors"
    (( SLO_ENFORCE==1 )) && exit 1
  fi
  echo
}

# Simple one-shot concurrency timing (fire N in parallel once)
run_concurrent_benchmark() {
  local method="$1"; shift || true
  local -a PARAMS=( "$@" )
  local c="$CONCURRENT"

  echo -e "${BLUE}Concurrent benchmark $method (${c}x)…${NC}"
  local start=$(date +%s%N)
  local pids=()
  for ((i=1;i<=c;i++)); do 
    if (( ${#PARAMS[@]} > 0 )); then
      ( rpc "$method" "${PARAMS[@]}" >/dev/null 2>&1 ) & pids+=($!)
    else
      ( rpc "$method" >/dev/null 2>&1 ) & pids+=($!)
    fi
  done
  for pid in "${pids[@]}"; do wait "$pid"; done
  local end=$(date +%s%N)
  local total=$(( (end - start) / 1000000 ))
  echo -e "${GREEN}✅ ${method}: ${total}ms total | ~$(( total / c ))ms avg/request${NC}"
  echo
}

# Concurrency sweep to find knee points
concurrency_sweep() {
  local method="$1"; shift || true
  local -a PARAMS=( "$@" )
  echo -e "${BLUE}Concurrency sweep for $method (1,2,4,8,16)…${NC}"
  for c in 1 2 4 8 16; do
    local start=$(date +%s%N)
    local pids=()
    for ((i=1;i<=c;i++)); do 
      if (( ${#PARAMS[@]} > 0 )); then
        ( rpc "$method" "${PARAMS[@]}" >/dev/null 2>&1 ) & pids+=($!)
      else
        ( rpc "$method" >/dev/null 2>&1 ) & pids+=($!)
      fi
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
    local end=$(date +%s%N)
    local total=$(( (end - start) / 1000000 ))
    local per=$(( total / c ))
    echo "  c=${c}: total=${total}ms (~${per}ms/req)"
  done
  echo
}

# Memory usage (RSS) sampling during run
monitor_memory() {
  local pid
  pid=$(pgrep -f "dinerod.*-datadir.*$DATADIR" | head -1)
  if [[ -z "${pid:-}" ]]; then
    echo -e "${YELLOW}Warning: Could not find dinerod process for memory monitoring${NC}"
    return 0
  fi
  echo -e "${BLUE}Monitoring memory (PID: $pid)…${NC}"
  local initial=$(ps -o rss= -p "$pid" 2>/dev/null || echo "0")
  local peak="$initial"
  echo "Initial memory: ${initial}KB"
  for ((i=1;i<=ITERATIONS;i++)); do
    local cur=$(ps -o rss= -p "$pid" 2>/dev/null || echo "0")
    (( cur > peak )) && peak="$cur"
    (( i % 20 == 0 )) && echo "  Iteration $i: ${cur}KB"
    sleep 0.1
  done
  echo "Peak memory:  ${peak}KB"
  echo "Memory Δ:     $((peak - initial))KB"
  echo
}

main() {
  echo -e "${GREEN}🧪 Dinero RPC Performance Benchmark${NC}"
  echo "=========================================="
  echo "Iterations: $ITERATIONS"
  echo "Concurrent: $CONCURRENT"
  echo "RPC URL:    $RPC_URL"
  echo "Data dir:   $DATADIR"
  echo "Network:    $NETWORK"
  echo "SLO p95<=${SLO_P95_MS}ms, p99<=${SLO_P99_MS}ms (enforce=${SLO_ENFORCE})"
  [[ -n "$JSON_OUT" ]] && echo "JSON out:   $JSON_OUT"
  echo "=========================================="
  echo

  # Init JSON file if requested
  if [[ -n "$JSON_OUT" ]]; then echo "[]" > "$JSON_OUT"; fi

  # Current state
  echo -e "${BLUE}Getting current blockchain state...${NC}"
  height="$(rpc getblockcount | jq -r '.result' 2>/dev/null || echo "0")"
  echo "Current height: $height"
  if [[ "$height" == "0" ]]; then
    echo -e "${YELLOW}Warning: height=0; some block tests may be skipped${NC}"
  fi
  echo

  echo -e "${GREEN}Starting RPC benchmarks...${NC}"
  echo

  # Core light calls
  bench_series "getblockcount"
  bench_series "getblockchaininfo"
  bench_series "getmininginfo"

  # Block-related methods (if chain has data)
  if [[ "$height" != "0" ]]; then
    # getblockhash for current tip (numeric param)
    bench_series "getblockhash" "$height"

    # pick hash at tip for getblock(verbose=true)
    tip_hash="$(rpc getblockhash "$height" | jq -r '.result')"
    if [[ -n "$tip_hash" && "$tip_hash" != "null" ]]; then
      bench_series "getblock" "$tip_hash" true
    fi
  fi

  # One-shot concurrency tests (fixed c = CONCURRENT)
  echo -e "${GREEN}Running concurrent benchmarks...${NC}"
  echo
  run_concurrent_benchmark "getblockcount"
  run_concurrent_benchmark "getblockchaininfo"

  # Concurrency sweep (optional, quick)
  concurrency_sweep "getblockcount"

  # Memory monitoring
  monitor_memory

  echo -e "${GREEN}📊 Benchmark Summary complete${NC}"
  [[ -n "$JSON_OUT" ]] && echo "JSON metrics: $JSON_OUT"
  echo
  echo -e "${GREEN}🎉 Benchmark completed!${NC}"
}

main "$@"
