#!/usr/bin/env bash
# wait_ready.sh - Universal health probe for DineroCoin daemon
# Usage: wait_ready.sh [URL] [DATADIR] [TIMEOUT]

wait_ready() {
  local url=${1:-http://127.0.0.1:20998/}
  local datadir=${2:-./}
  local timeout=${3:-60}
  
  # Extract base URL without trailing slash for cookie path
  local base_url=$(echo "$url" | sed 's|/$||')
  local cookie_file="$datadir/.cookie"
  
  for i in $(seq 1 $timeout); do
    if [[ -f "$cookie_file" ]]; then
      # Use cookie authentication
      local auth=$(cat "$cookie_file")
      if curl -sf --max-time 2 \
           --user "$auth" \
           -H 'Content-Type: application/json' \
           --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
           "$url" >/dev/null 2>&1; then
        return 0
      fi
    else
      # Fallback to basic connectivity test
      if curl -sf --max-time 2 "$base_url/health" >/dev/null 2>&1; then
        return 0
      fi
    fi
    sleep 1
  done
  return 1
}

# If called directly, run the function
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  wait_ready "$@"
fi
