#!/bin/bash
# Universal RPC helper that always uses the GUI's daemon instance

# Function to find the current GUI daemon's nodeinfo
find_gui_nodeinfo() {
    # First try the stable symlink
    local stable_path="$HOME/Library/Application Support/DineroCoin/Dinero All-in-One/current-nodeinfo.json"
    if [[ -f "$stable_path" ]]; then
        echo "$stable_path"
        return 0
    fi
    
    # Fallback: find GUI process and its nodeinfo
    local gui_pid=$(pgrep -f 'dinero-all-in-one.app/Contents/MacOS/dinero-all-in-one' | tail -1)
    if [[ -n "$gui_pid" ]]; then
        local nodeinfo=$(find /private/var/folders -name "dinero-nodeinfo-${gui_pid}.json" -print -quit 2>/dev/null)
        if [[ -f "$nodeinfo" ]]; then
            echo "$nodeinfo"
            return 0
        fi
    fi
    
    echo "❌ No GUI daemon found. Please start the Dinero All-in-One app first." >&2
    return 1
}

# Initialize connection info
init_connection() {
    local nodeinfo
    if ! nodeinfo=$(find_gui_nodeinfo); then
        return 1
    fi
    
    RPC_URL=$(jq -r '.rpc.url' "$nodeinfo" 2>/dev/null)
    APPDATA=$(jq -r '.datadir' "$nodeinfo" 2>/dev/null)
    
    if [[ "$RPC_URL" == "null" || "$APPDATA" == "null" ]]; then
        echo "❌ Invalid nodeinfo format" >&2
        return 1
    fi
    
    COOKIE_FILE="$APPDATA/.cookie"
    if [[ ! -f "$COOKIE_FILE" ]]; then
        echo "❌ Cookie file not found: $COOKIE_FILE" >&2
        return 1
    fi
    
    COOKIE=$(sed -e 's/^__cookie__://' -e 's/[\r\n]//g' "$COOKIE_FILE")
    
    if [[ ${#COOKIE} -lt 10 ]]; then
        echo "❌ Invalid cookie format" >&2
        return 1
    fi
}

# RPC call function
din() {
    local method="$1"
    local params="${2:-{}}"
    
    if [[ -z "$RPC_URL" || -z "$COOKIE" ]]; then
        if ! init_connection; then
            return 1
        fi
    fi
    
    curl -s --fail -u "__cookie__:${COOKIE}" \
        -H 'Content-Type: application/json' \
        -d "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params}}" \
        "$RPC_URL"
}

# Check dependencies
check_deps() {
    if ! command -v jq >/dev/null 2>&1; then
        echo "❌ jq is required. Install with: brew install jq" >&2
        return 1
    fi
    
    if ! command -v curl >/dev/null 2>&1; then
        echo "❌ curl is required" >&2
        return 1
    fi
}

# Main execution
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    # Script is being run directly
    if ! check_deps; then
        exit 1
    fi
    
    if [[ $# -eq 0 ]]; then
        echo "Usage: $0 <method> [params]"
        echo "Examples:"
        echo "  $0 getblockcount"
        echo "  $0 wallet.getnewaddress"
        echo "  $0 address.list '{\"include_labels\":true}'"
        exit 1
    fi
    
    din "$@" | jq -r '.'
else
    # Script is being sourced
    if ! check_deps; then
        return 1
    fi
    
    if ! init_connection; then
        return 1
    fi
    
    echo "✅ RPC helper loaded - use din() function"
    echo "   RPC: $RPC_URL"
    echo "   Cookie: ${#COOKIE} chars"
fi
