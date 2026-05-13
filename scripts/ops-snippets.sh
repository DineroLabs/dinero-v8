#!/bin/bash
# DineroCoin CLI v1.0.0 - Operational Snippets
# Copy-paste ready commands for production operations

# =============================================================================
# READINESS GATES
# =============================================================================

# Basic readiness check
readiness_check() {
    local profile="${1:-prod}"
    echo "Checking readiness for profile: $profile"
    dinero-cli --profile "$profile" --wait-ready --timeout 60 || {
        echo "❌ Node not ready (exit code: $?)"
        exit 69
    }
    echo "✅ Node ready"
}

# Comprehensive health check
health_check() {
    local profile="${1:-prod}"
    echo "=== Health Check: $profile ==="
    
    # Node readiness
    dinero-cli --profile "$profile" --wait-ready --timeout 30 || {
        echo "❌ Node unreachable"
        return 69
    }
    
    # Basic info
    local info=$(dinero-cli --profile "$profile" --format json --nodeinfo)
    local blocks=$(echo "$info" | jq -r '.data.blocks // "unknown"')
    local connections=$(echo "$info" | jq -r '.data.connections // "unknown"')
    
    echo "✅ Blocks: $blocks"
    echo "✅ Connections: $connections"
    
    # Wallet balance (if wallet configured)
    if dinero-cli --profile "$profile" --format json wallet balance >/dev/null 2>&1; then
        local balance=$(dinero-cli --profile "$profile" --format json wallet balance | jq -r '.data.total // "unknown"')
        echo "✅ Wallet Balance: $balance DIN"
    fi
}

# =============================================================================
# PAGED EXPORTS (DETERMINISTIC)
# =============================================================================

# Export complete transaction history
export_wallet_history() {
    local profile="${1:-prod}"
    local output_file="${2:-history.ndjson}"
    local limit=1000
    local offset=0
    
    echo "Exporting wallet history to $output_file..."
    > "$output_file"  # Clear file
    
    while true; do
        echo "Fetching offset $offset (limit $limit)..."
        
        local response=$(dinero-cli --profile "$profile" --format json wallet history --limit "$limit" --offset "$offset")
        local ok=$(echo "$response" | jq -r '.ok // false')
        
        if [[ "$ok" != "true" ]]; then
            echo "❌ Error: $(echo "$response" | jq -r '.error.message // "Unknown error"')"
            return 1
        fi
        
        # Append data to file
        echo "$response" | jq -c '.data[]' >> "$output_file"
        
        # Check if more pages available
        local has_more=$(echo "$response" | jq -r '.page.has_more // false')
        if [[ "$has_more" != "true" ]]; then
            break
        fi
        
        # Get next offset
        offset=$(echo "$response" | jq -r '.page.next_offset')
        if [[ "$offset" == "null" ]]; then
            break
        fi
    done
    
    local total_lines=$(wc -l < "$output_file")
    echo "✅ Exported $total_lines transactions to $output_file"
}

# Export confirmed UTXOs
export_utxos() {
    local profile="${1:-prod}"
    local output_file="${2:-utxos.ndjson}"
    local min_amount="${3:-0.01}"
    local limit=500
    local offset=0
    
    echo "Exporting UTXOs (min: $min_amount DIN) to $output_file..."
    > "$output_file"
    
    while true; do
        echo "Fetching UTXO offset $offset (limit $limit)..."
        
        local response=$(dinero-cli --profile "$profile" --format json wallet utxos \
            --confirmed-only --min-amount "$min_amount" --limit "$limit" --offset "$offset")
        
        local ok=$(echo "$response" | jq -r '.ok // false')
        if [[ "$ok" != "true" ]]; then
            echo "❌ Error: $(echo "$response" | jq -r '.error.message // "Unknown error"')"
            return 1
        fi
        
        echo "$response" | jq -c '.data[]' >> "$output_file"
        
        local has_more=$(echo "$response" | jq -r '.page.has_more // false')
        if [[ "$has_more" != "true" ]]; then
            break
        fi
        
        offset=$(echo "$response" | jq -r '.page.next_offset')
        if [[ "$offset" == "null" ]]; then
            break
        fi
    done
    
    local total_lines=$(wc -l < "$output_file")
    echo "✅ Exported $total_lines UTXOs to $output_file"
}

# =============================================================================
# MONITORING LOOPS
# =============================================================================

# Balance monitoring with threshold alerts
monitor_balance() {
    local profile="${1:-prod}"
    local threshold="${2:-1000.0}"
    local interval="${3:-300}"  # 5 minutes
    
    echo "Monitoring balance (threshold: $threshold DIN, interval: ${interval}s)"
    
    while true; do
        local timestamp=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
        local response=$(dinero-cli --profile "$profile" --format json wallet balance 2>/dev/null)
        
        if [[ $? -eq 0 ]]; then
            local balance=$(echo "$response" | jq -r '.data.total // "unknown"')
            echo "[$timestamp] Balance: $balance DIN"
            
            # Threshold check
            if command -v bc >/dev/null 2>&1; then
                if (( $(echo "$balance < $threshold" | bc -l) )); then
                    echo "⚠️  WARNING: Balance $balance below threshold $threshold"
                    # Add alerting logic here (email, webhook, etc.)
                fi
            fi
        else
            echo "[$timestamp] ❌ Failed to get balance"
        fi
        
        sleep "$interval"
    done
}

# Network monitoring
monitor_network() {
    local profile="${1:-prod}"
    local interval="${2:-60}"  # 1 minute
    
    echo "Monitoring network status (interval: ${interval}s)"
    
    while true; do
        local timestamp=$(date -u '+%Y-%m-%d %H:%M:%S UTC')
        
        # Node info
        local nodeinfo=$(dinero-cli --profile "$profile" --format json --nodeinfo 2>/dev/null)
        if [[ $? -eq 0 ]]; then
            local blocks=$(echo "$nodeinfo" | jq -r '.data.blocks // "unknown"')
            local connections=$(echo "$nodeinfo" | jq -r '.data.connections // "unknown"')
            echo "[$timestamp] Blocks: $blocks, Connections: $connections"
        else
            echo "[$timestamp] ❌ Node unreachable"
        fi
        
        # Peer count
        local peers=$(dinero-cli --profile "$profile" --format json net peers --limit 1 2>/dev/null)
        if [[ $? -eq 0 ]]; then
            local peer_count=$(echo "$peers" | jq -r '.page.returned // 0')
            echo "[$timestamp] Active peers: $peer_count"
        fi
        
        sleep "$interval"
    done
}

# =============================================================================
# EMERGENCY PROCEDURES
# =============================================================================

# Quick failover to backup node
failover_to_backup() {
    local profile="${1:-prod}"
    local backup_url="${2:-https://backup-node.example.com:20998}"
    
    echo "Testing failover to backup node..."
    
    # Test primary
    if dinero-cli --profile "$profile" --timeout 5 --retries 1 --nodeinfo >/dev/null 2>&1; then
        echo "✅ Primary node responsive, no failover needed"
        return 0
    fi
    
    echo "❌ Primary node down, testing backup..."
    
    # Test backup
    if dinero-cli --profile "$profile" --rpc-url "$backup_url" --timeout 10 --nodeinfo >/dev/null 2>&1; then
        echo "✅ Backup node responsive"
        echo "To use backup: dinero-cli --profile $profile --rpc-url $backup_url [command]"
        return 0
    else
        echo "❌ Backup node also unreachable"
        return 69
    fi
}

# Data consistency check
consistency_check() {
    local profile="${1:-prod}"
    
    echo "Running data consistency check..."
    
    # Get recent transaction
    local recent_tx=$(dinero-cli --profile "$profile" --format json wallet history --limit 1 2>/dev/null)
    if [[ $? -ne 0 ]]; then
        echo "❌ Cannot fetch recent transactions"
        return 1
    fi
    
    local txid=$(echo "$recent_tx" | jq -r '.data[0].txid // null')
    if [[ "$txid" == "null" ]]; then
        echo "ℹ️  No transactions found"
        return 0
    fi
    
    # Verify transaction exists in blockchain
    if dinero-cli --profile "$profile" tx get "$txid" >/dev/null 2>&1; then
        echo "✅ Recent transaction $txid verified in blockchain"
    else
        echo "❌ Transaction data inconsistency detected: $txid"
        return 1
    fi
}

# =============================================================================
# USAGE EXAMPLES
# =============================================================================

show_usage() {
    cat << 'EOF'
DineroCoin CLI Operational Snippets

Usage:
    source scripts/ops-snippets.sh

Functions:
    readiness_check [profile]           - Check if node is ready
    health_check [profile]              - Comprehensive health check
    export_wallet_history [profile] [file] - Export complete transaction history
    export_utxos [profile] [file] [min] - Export confirmed UTXOs
    monitor_balance [profile] [threshold] [interval] - Monitor balance with alerts
    monitor_network [profile] [interval] - Monitor network status
    failover_to_backup [profile] [backup_url] - Test failover to backup node
    consistency_check [profile]         - Check data consistency

Examples:
    readiness_check prod
    health_check prod
    export_wallet_history prod transactions.ndjson
    export_utxos prod utxos.ndjson 0.01
    monitor_balance prod 1000.0 300
    failover_to_backup prod https://backup.example.com:20998

EOF
}

# Show usage if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    show_usage
fi
