#!/usr/bin/env bash
# DineroCoin RPC Examples
# Phase Z.3: RPC API Documentation
#
# Purpose: Demonstrate common RPC operations
# Usage: ./contrib/rpc-examples.sh [example_name]
#
# See also:
# - docs/RPC_COMPATIBILITY.md - API stability guarantees
# - docs/RPC_API.md - Complete API reference

set -euo pipefail

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}$1${NC}"
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_example() {
    echo -e "${BLUE}Example:${NC} $1"
    echo -e "${YELLOW}$ $2${NC}"
}

# Helper function to call RPC
rpc_call() {
    dinero-cli "$@"
}

# ============================================================================
# RPC Introspection Examples
# ============================================================================

example_introspection() {
    print_header "RPC Introspection"

    print_example "Get server capabilities" "dinero-cli rpc.capabilities"
    rpc_call rpc.capabilities || true
    echo ""

    print_example "List all RPC methods" "dinero-cli rpc.listmethods"
    rpc_call rpc.listmethods || true
    echo ""

    print_example "Get help for specific method" "dinero-cli rpc.help wallet.getbalance"
    rpc_call rpc.help wallet.getbalance || true
    echo ""

    print_example "Get API version hash" "dinero-cli rpc.apihash"
    rpc_call rpc.apihash || true
    echo ""
}

# ============================================================================
# Blockchain Query Examples
# ============================================================================

example_blockchain() {
    print_header "Blockchain Queries"

    print_example "Get blockchain info" "dinero-cli blockchain.getinfo"
    rpc_call blockchain.getinfo || rpc_call getblockchaininfo || true
    echo ""

    print_example "Get current block count" "dinero-cli blockchain.getblockcount"
    rpc_call blockchain.getblockcount || rpc_call getblockcount || true
    echo ""

    print_example "Get best block hash" "dinero-cli blockchain.getbestblockhash"
    BEST_HASH=$(rpc_call blockchain.getbestblockhash || rpc_call getbestblockhash || echo "")
    echo "$BEST_HASH"
    echo ""

    if [ -n "$BEST_HASH" ]; then
        print_example "Get block by hash" "dinero-cli blockchain.getblock \"$BEST_HASH\""
        rpc_call blockchain.getblock "$BEST_HASH" || rpc_call getblock "$BEST_HASH" || true
        echo ""
    fi

    print_example "Get blockchain difficulty" "dinero-cli blockchain.getdifficulty"
    rpc_call blockchain.getdifficulty || rpc_call getdifficulty || true
    echo ""
}

# ============================================================================
# Wallet Examples
# ============================================================================

example_wallet() {
    print_header "Wallet Operations"

    print_example "Get wallet balance" "dinero-cli wallet.getbalance"
    rpc_call wallet.getbalance || rpc_call getbalance || true
    echo ""

    print_example "Get wallet info" "dinero-cli wallet.getinfo"
    rpc_call wallet.getinfo || rpc_call getwalletinfo || true
    echo ""

    print_example "Generate new address" "dinero-cli wallet.getnewaddress"
    ADDRESS=$(rpc_call wallet.getnewaddress || rpc_call getnewaddress || echo "")
    echo "$ADDRESS"
    echo ""

    print_example "Generate new address with label" "dinero-cli wallet.getnewaddress \"My Label\""
    rpc_call wallet.getnewaddress "Payment Address" || rpc_call getnewaddress "Payment Address" || true
    echo ""

    print_example "List recent transactions" "dinero-cli wallet.listtransactions 10"
    rpc_call wallet.listtransactions 10 || rpc_call listtransactions 10 || true
    echo ""

    print_example "List unspent outputs" "dinero-cli wallet.listunspent"
    rpc_call wallet.listunspent || rpc_call listunspent || true
    echo ""

    if [ -n "$ADDRESS" ]; then
        print_example "Validate address" "dinero-cli wallet.validateaddress \"$ADDRESS\""
        rpc_call wallet.validateaddress "$ADDRESS" || rpc_call validateaddress "$ADDRESS" || true
        echo ""
    fi
}

# ============================================================================
# Mining Examples
# ============================================================================

example_mining() {
    print_header "Mining Control"

    print_example "Get mining info" "dinero-cli mining.getinfo"
    rpc_call mining.getinfo || rpc_call getmininginfo || true
    echo ""

    print_example "Get mining address" "dinero-cli mining.getaddress"
    MINING_ADDR=$(rpc_call mining.getaddress 2>/dev/null || echo "")
    echo "${MINING_ADDR:-Not set}"
    echo ""

    # Only show start/stop examples (don't actually start mining)
    print_example "Start CPU mining (4 threads)" "dinero-cli mining.start 4"
    echo "(Not executed - example only)"
    echo ""

    print_example "Stop CPU mining" "dinero-cli mining.stop"
    echo "(Not executed - example only)"
    echo ""

    print_example "Set mining payout address" "dinero-cli mining.setaddress \"DIN1A2B3...\""
    echo "(Not executed - example only)"
    echo ""
}

# ============================================================================
# P2P Network Examples
# ============================================================================

example_p2p() {
    print_header "P2P Network"

    print_example "Get peer info" "dinero-cli p2p.getpeerinfo"
    rpc_call p2p.getpeerinfo || rpc_call getpeerinfo || true
    echo ""

    print_example "Get sync status" "dinero-cli p2p.getsyncstatus"
    rpc_call p2p.getsyncstatus 2>/dev/null || echo "{\"syncing\": false, \"info\": \"Method may vary by version\"}"
    echo ""

    print_example "Add peer node" "dinero-cli addnode \"192.168.1.100:20999\" \"add\""
    echo "(Not executed - example only)"
    echo ""

    print_example "Remove peer node" "dinero-cli addnode \"192.168.1.100:20999\" \"remove\""
    echo "(Not executed - example only)"
    echo ""
}

# ============================================================================
# Transaction Examples
# ============================================================================

example_transactions() {
    print_header "Transactions"

    print_example "Send to address" "dinero-cli wallet.sendtoaddress \"DIN1A2B3...\" 10.5"
    echo "(Not executed - example only)"
    echo ""

    print_example "Send to address with comment" "dinero-cli wallet.sendtoaddress \"DIN1A2B3...\" 10.5 \"Payment to Alice\""
    echo "(Not executed - example only)"
    echo ""

    print_example "Send to multiple addresses" "dinero-cli wallet.sendmany '{\"DIN1A2...\": 10.0, \"DIN3B4...\": 5.0}'"
    echo "(Not executed - example only)"
    echo ""

    print_example "Get transaction by txid" "dinero-cli blockchain.gettransaction \"txid...\""
    echo "(Requires txid - example only)"
    echo ""

    print_example "Get raw transaction" "dinero-cli wallet.getrawtransaction \"txid...\""
    echo "(Requires txid - example only)"
    echo ""
}

# ============================================================================
# Mempool Examples
# ============================================================================

example_mempool() {
    print_header "Mempool Management"

    print_example "Get mempool info" "dinero-cli mempool.getinfo"
    rpc_call mempool.getinfo || rpc_call getmempoolinfo || true
    echo ""

    print_example "Get raw mempool (brief)" "dinero-cli mempool.getrawmempool"
    rpc_call mempool.getrawmempool || rpc_call getrawmempool || true
    echo ""

    print_example "Get raw mempool (verbose)" "dinero-cli mempool.getrawmempool true"
    rpc_call mempool.getrawmempool true || rpc_call getrawmempool true || true
    echo ""

    print_example "Estimate fee for 6 block confirmation" "dinero-cli mempool.estimatefee 6"
    rpc_call mempool.estimatefee 6 || rpc_call estimatesmartfee 6 || true
    echo ""
}

# ============================================================================
# Wallet Encryption Examples
# ============================================================================

example_encryption() {
    print_header "Wallet Encryption & Security"

    print_example "Encrypt wallet (first time)" "dinero-cli wallet.encrypt \"my_strong_passphrase\""
    echo "(Not executed - irreversible operation)"
    echo ""

    print_example "Unlock wallet for 5 minutes" "dinero-cli wallet.unlock \"my_passphrase\" 300"
    echo "(Not executed - requires encrypted wallet)"
    echo ""

    print_example "Lock wallet" "dinero-cli wallet.lock"
    echo "(Not executed - requires encrypted wallet)"
    echo ""

    print_example "Change wallet passphrase" "dinero-cli wallet.passphrasechange \"old_pass\" \"new_pass\""
    echo "(Not executed - requires encrypted wallet)"
    echo ""

    print_example "Backup wallet" "dinero-cli wallet.backup \"/backup/wallet-2025-12-31.dat\""
    echo "(Not executed - example only)"
    echo ""
}

# ============================================================================
# Advanced Examples
# ============================================================================

example_advanced() {
    print_header "Advanced Operations"

    print_example "Get OpenRPC schema" "dinero-cli rpc.openrpc > openrpc-schema.json"
    echo "(Not executed - generates large file)"
    echo ""

    print_example "Get schema for specific method" "dinero-cli rpc.schema wallet.sendtoaddress"
    rpc_call rpc.schema wallet.sendtoaddress 2>/dev/null || echo "{\"info\": \"Method may vary by version\"}"
    echo ""

    print_example "List methods by namespace" "dinero-cli rpc.namespaces"
    rpc_call rpc.namespaces 2>/dev/null || echo "{\"info\": \"Method may vary by version\"}"
    echo ""

    print_example "Get server health" "dinero-cli rpc.health"
    rpc_call rpc.health 2>/dev/null || echo "{\"status\": \"Method may vary by version\"}"
    echo ""
}

# ============================================================================
# Curl Examples (without dinero-cli)
# ============================================================================

example_curl() {
    print_header "Using curl (Direct HTTP)"

    echo -e "${BLUE}Example:${NC} Get blockchain info using curl with cookie auth"
    echo -e "${YELLOW}$ curl --user \"\$(cat ~/.dinero/.cookie)\" \\"
    echo "  --data-binary '{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"blockchain.getinfo\",\"params\":[]}' \\"
    echo "  -H \"content-type: application/json\" \\"
    echo -e "  http://127.0.0.1:20998/${NC}"
    echo ""

    echo -e "${BLUE}Example:${NC} Get wallet balance using curl with static credentials"
    echo -e "${YELLOW}$ curl --user \"username:password\" \\"
    echo "  --data-binary '{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"wallet.getbalance\",\"params\":[]}' \\"
    echo "  -H \"content-type: application/json\" \\"
    echo -e "  http://127.0.0.1:20998/${NC}"
    echo ""

    echo -e "${BLUE}Example:${NC} Send DIN using curl"
    echo -e "${YELLOW}$ curl --user \"username:password\" \\"
    echo "  --data-binary '{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"wallet.sendtoaddress\",\"params\":[\"DIN1A2B...\",10.5]}' \\"
    echo "  -H \"content-type: application/json\" \\"
    echo -e "  http://127.0.0.1:20998/${NC}"
    echo ""
}

# ============================================================================
# Python Examples
# ============================================================================

example_python() {
    print_header "Using Python"

    cat <<'EOF'
Example: Python RPC client

```python
import requests
import json

class DineroCoinRPC:
    def __init__(self, url="http://127.0.0.1:20998", user="admin", password="password"):
        self.url = url
        self.auth = (user, password)

    def call(self, method, params=[]):
        payload = {
            "jsonrpc": "2.0",
            "id": "1",
            "method": method,
            "params": params
        }
        response = requests.post(self.url, json=payload, auth=self.auth)
        result = response.json()

        if "error" in result and result["error"]:
            raise Exception(f"RPC Error: {result['error']}")

        return result.get("result")

# Usage
rpc = DineroCoinRPC()

# Get blockchain info
info = rpc.call("blockchain.getinfo")
print(f"Block height: {info['blocks']}")

# Get wallet balance
balance = rpc.call("wallet.getbalance")
print(f"Balance: {balance['balance']} DIN")

# Generate new address
address = rpc.call("wallet.getnewaddress", ["My Label"])
print(f"New address: {address}")

# Send transaction (be careful!)
# txid = rpc.call("wallet.sendtoaddress", ["DIN1A2B3...", 10.5])
# print(f"Transaction: {txid}")
```
EOF
}

# ============================================================================
# Main Menu
# ============================================================================

show_menu() {
    print_header "DineroCoin RPC Examples"
    echo ""
    echo "Usage: ./contrib/rpc-examples.sh [example]"
    echo ""
    echo "Available examples:"
    echo "  introspection  - RPC introspection and discovery"
    echo "  blockchain     - Blockchain queries"
    echo "  wallet         - Wallet operations"
    echo "  mining         - Mining control"
    echo "  p2p            - P2P network management"
    echo "  transactions   - Transaction examples"
    echo "  mempool        - Mempool queries"
    echo "  encryption     - Wallet security"
    echo "  advanced       - Advanced RPC features"
    echo "  curl           - Using curl directly"
    echo "  python         - Python integration examples"
    echo "  all            - Run all examples"
    echo ""
    echo "Examples:"
    echo "  ./contrib/rpc-examples.sh blockchain"
    echo "  ./contrib/rpc-examples.sh wallet"
    echo "  ./contrib/rpc-examples.sh all"
    echo ""
}

# Main execution
if [ $# -eq 0 ]; then
    show_menu
    exit 0
fi

case "$1" in
    introspection)
        example_introspection
        ;;
    blockchain)
        example_blockchain
        ;;
    wallet)
        example_wallet
        ;;
    mining)
        example_mining
        ;;
    p2p)
        example_p2p
        ;;
    transactions)
        example_transactions
        ;;
    mempool)
        example_mempool
        ;;
    encryption)
        example_encryption
        ;;
    advanced)
        example_advanced
        ;;
    curl)
        example_curl
        ;;
    python)
        example_python
        ;;
    all)
        example_introspection
        example_blockchain
        example_wallet
        example_mining
        example_p2p
        example_transactions
        example_mempool
        example_encryption
        example_advanced
        example_curl
        example_python
        ;;
    *)
        echo "Unknown example: $1"
        echo ""
        show_menu
        exit 1
        ;;
esac

exit 0
