#!/bin/bash
# Shared wait functions for Dinero testing

# Wait for cookie file to be created
wait_cookie() {
    local cookie_path="$1"
    local max_attempts="${2:-100}"
    
    for i in $(seq 1 $max_attempts); do
        if [[ -s "$cookie_path" ]]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Wait for RPC to return HTTP 200
wait_rpc_200() {
    local nodeinfo_path="$1"
    local cookie_path="$2"
    local max_attempts="${3:-120}"
    
    if [[ ! -s "$nodeinfo_path" ]]; then
        echo "ERROR: nodeinfo.json not found: $nodeinfo_path" >&2
        return 1
    fi
    
    if [[ ! -s "$cookie_path" ]]; then
        echo "ERROR: cookie file not found: $cookie_path" >&2
        return 1
    fi
    
    # Extract RPC port (handle both formats: .rpc and .rpc.port)
    local rpc_port
    if command -v jq >/dev/null; then
        rpc_port=$(jq -r '.rpc.port // .rpc' "$nodeinfo_path" 2>/dev/null)
    else
        # Fallback without jq
        rpc_port=$(python3 -c "
import json,sys
data = json.load(open('$nodeinfo_path'))
if isinstance(data.get('rpc'), dict):
    print(data['rpc']['port'])
else:
    print(data.get('rpc', 0))
" 2>/dev/null)
    fi
    
    if [[ -z "$rpc_port" || "$rpc_port" == "null" || "$rpc_port" == "0" ]]; then
        echo "ERROR: Invalid RPC port in nodeinfo.json: $rpc_port" >&2
        return 1
    fi
    
    local auth
    auth=$(tr -d '\r\n' < "$cookie_path")
    
    for i in $(seq 1 $max_attempts); do
        local http_code
        http_code=$(curl -fsS -w '%{http_code}' -o /dev/null \
            --max-time 2 \
            --user "$auth" \
            -H 'content-type: application/json' \
            --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
            "http://127.0.0.1:$rpc_port" 2>/dev/null || echo "000")
        
        if [[ "$http_code" == "200" ]]; then
            return 0
        fi
        sleep 0.25
    done
    
    echo "ERROR: RPC not responding with HTTP 200 after $max_attempts attempts" >&2
    return 1
}

# Wait for P2P port to be listening
wait_p2p_listen() {
    local nodeinfo_path="$1"
    local max_attempts="${2:-120}"
    
    if [[ ! -s "$nodeinfo_path" ]]; then
        echo "ERROR: nodeinfo.json not found: $nodeinfo_path" >&2
        return 1
    fi
    
    # Extract P2P port (handle both formats: .p2p and .p2p.port)
    local p2p_port
    if command -v jq >/dev/null; then
        p2p_port=$(jq -r '.p2p.port // .p2p' "$nodeinfo_path" 2>/dev/null)
    else
        # Fallback without jq
        p2p_port=$(python3 -c "
import json,sys
data = json.load(open('$nodeinfo_path'))
if isinstance(data.get('p2p'), dict):
    print(data['p2p']['port'])
else:
    print(data.get('p2p', 0))
" 2>/dev/null)
    fi
    
    if [[ -z "$p2p_port" || "$p2p_port" == "null" || "$p2p_port" == "0" ]]; then
        echo "ERROR: Invalid P2P port in nodeinfo.json: $p2p_port" >&2
        return 1
    fi
    
    for i in $(seq 1 $max_attempts); do
        if lsof -nP -iTCP:"$p2p_port" -sTCP:LISTEN >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    
    echo "ERROR: P2P port $p2p_port not listening after $max_attempts attempts" >&2
    return 1
}

# Cross-platform timeout wrapper
timeout_wrap() {
    local timeout_seconds="$1"
    shift
    
    if command -v timeout >/dev/null; then
        timeout "$timeout_seconds" "$@"
    elif command -v gtimeout >/dev/null; then
        gtimeout "$timeout_seconds" "$@"
    else
        python3 -c "
import subprocess, sys, signal, time
timeout_sec = int(sys.argv[1])
cmd = sys.argv[2:]
p = subprocess.Popen(cmd)
try:
    p.wait(timeout=timeout_sec)
    sys.exit(p.returncode)
except subprocess.TimeoutExpired:
    p.send_signal(signal.SIGINT)
    time.sleep(0.3)
    if p.poll() is None:
        p.kill()
    sys.exit(124)
" "$timeout_seconds" "$@"
    fi
}

# Test P2P connectivity
test_p2p_connect() {
    local host="$1"
    local port="$2"
    
    if command -v nc >/dev/null; then
        nc -z "$host" "$port" 2>/dev/null
    else
        python3 -c "
import socket, sys
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2)
    s.connect(('$host', $port))
    s.close()
    sys.exit(0)
except:
    sys.exit(1)
"
    fi
}
