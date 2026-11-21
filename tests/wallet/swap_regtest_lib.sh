# Sourced by the swap regtest scripts. A port that already answers belongs to
# someone else — possibly a real mainnet or testnet node on this machine — so
# the scripts take only a block of free ports, and check that each daemon they
# started reports regtest before sending it anything that writes.

swap_port_busy() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

swap_free_base() {  # <span> -> a base in 30000-38999 with base..base+span all free
  local base port _
  for _ in $(seq 1 50); do
    base=$((30000 + RANDOM % 9000))
    for port in $(seq "$base" $((base + $1))); do swap_port_busy "$port" && continue 2; done
    echo "$base"; return 0
  done
  echo "no free block of $(($1 + 1)) ports" >&2; return 1
}

swap_require_regtest() {  # <label> <getblockchaininfo json>
  local chain
  chain=$(python3 -c 'import sys,json; print(json.loads(sys.argv[1]).get("chain",""))' "$2" 2>/dev/null || true)
  [[ "$chain" == regtest ]] || { echo "ABORT: $1 reports chain '$chain', not regtest: not the daemon this test started" >&2; exit 1; }
}
