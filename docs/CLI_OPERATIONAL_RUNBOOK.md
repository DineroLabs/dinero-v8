# DineroCoin CLI Operational Runbook

## Quick Reference

### Profile Setup
```bash
# Create profiles directory
mkdir -p ~/.dinero-cli

# Create profiles.json
cat > ~/.dinero-cli/profiles.json << 'EOF'
{
  "default": "dev",
  "profiles": {
    "dev": {
      "network": "regtest",
      "rpc_url": "http://127.0.0.1:20996",
      "datadir": "/tmp/din-dev",
      "wallet": "dev"
    },
    "prod": {
      "network": "main",
      "rpc_url": "https://node1.example.com:20998",
      "datadir": "~/.dinero",
      "read_timeout_ms": 30000
    }
  }
}
EOF
```

### Connection Testing
```bash
# Test node connectivity
dinero-cli --profile prod --wait-ready --timeout 60

# Verify profile settings
dinero-cli --profile prod --nodeinfo

# Test wallet access
dinero-cli --profile prod wallet balance
```

## Production Workflows

### Exchange Operations

#### Large UTXO Management
```bash
# Page through confirmed UTXOs
dinero-cli --profile prod wallet utxos \
  --confirmed-only \
  --min-amount 0.01 \
  --limit 500 \
  --format json

# Continue pagination
dinero-cli --profile prod wallet utxos \
  --confirmed-only \
  --min-amount 0.01 \
  --limit 500 \
  --offset 500 \
  --format json
```

#### Transaction History Analysis
```bash
# Recent transactions with filters
dinero-cli --profile prod wallet history \
  --since "2024-01-01T00:00:00Z" \
  --min-conf 6 \
  --limit 1000 \
  --format json

# Specific address activity
dinero-cli --profile prod wallet history \
  --address "DIN1abc..." \
  --limit 100 \
  --format json
```

#### Monitoring Scripts
```bash
#!/bin/bash
# balance-monitor.sh

PROFILE=${1:-prod}
THRESHOLD=${2:-1000.0}

balance=$(dinero-cli --profile $PROFILE --format json wallet balance | jq -r '.data.total')

if (( $(echo "$balance < $THRESHOLD" | bc -l) )); then
  echo "WARNING: Balance $balance below threshold $THRESHOLD"
  exit 1
fi

echo "Balance OK: $balance DIN"
```

### Node Operations

#### Health Checks
```bash
# Node readiness check
dinero-cli --profile prod --wait-ready --timeout 30 || {
  echo "Node not ready"
  exit 69
}

# Comprehensive status
dinero-cli --profile prod --nodeinfo --format json | jq '{
  blocks: .data.blocks,
  connections: .data.connections,
  difficulty: .data.difficulty,
  mempool_size: .data.mempool_size
}'
```

#### Peer Management
```bash
# Active peer analysis
dinero-cli --profile prod net peers \
  --state connected \
  --min-version 70016 \
  --limit 200 \
  --format json

# Connection quality check
dinero-cli --profile prod net peers --format json | \
  jq '.data[] | select(.ping_time > 1000) | {addr, ping_time}'
```

#### Mempool Monitoring
```bash
# High-fee transactions
dinero-cli --profile prod mempool list \
  --min-fee-rate 10.0 \
  --limit 100 \
  --format json

# Specific transaction tracking
dinero-cli --profile prod mempool list \
  --txid "abc123..." \
  --format json
```

## Development Workflows

### Local Testing
```bash
# Start regtest environment
dinero-cli --profile dev --wait-ready --timeout 10

# Create test wallet
dinero-cli --profile dev wallet create_wallet test_wallet

# Generate test blocks
dinero-cli --profile dev generate 101

# Test paging with small limits
dinero-cli --profile dev wallet history --limit 5 --format json
```

### Integration Testing
```bash
#!/bin/bash
# integration-test.sh

set -e

echo "Testing CLI integration..."

# Test basic connectivity
dinero-cli --profile dev --nodeinfo > /dev/null
echo "✓ Node connectivity"

# Test wallet operations
dinero-cli --profile dev wallet balance > /dev/null
echo "✓ Wallet access"

# Test paging
result=$(dinero-cli --profile dev --format json wallet history --limit 1)
has_page=$(echo "$result" | jq -r 'has("page")')
if [ "$has_page" = "true" ]; then
  echo "✓ Pagination metadata"
else
  echo "✗ Missing pagination metadata"
  exit 1
fi

echo "All tests passed!"
```

## Security Procedures

### Cookie Management
```bash
# Check cookie permissions (should be 600)
ls -la ~/.dinero/.cookie

# Fix permissions if needed
chmod 600 ~/.dinero/.cookie

# Never use insecure cookie in production
# This should ONLY be used in regtest/dev:
dinero-cli --profile dev --accept-insecure-cookie --nodeinfo
```

### Profile Security Audit
```bash
#!/bin/bash
# security-audit.sh

echo "Auditing CLI security..."

# Check profiles file permissions
if [ -f ~/.dinero-cli/profiles.json ]; then
  perms=$(stat -f "%A" ~/.dinero-cli/profiles.json 2>/dev/null || stat -c "%a" ~/.dinero-cli/profiles.json)
  if [ "$perms" != "600" ]; then
    echo "WARNING: profiles.json permissions should be 600, found $perms"
  fi
fi

# Check for insecure cookie usage in production profiles
if grep -q "accept_insecure_cookie.*true" ~/.dinero-cli/profiles.json 2>/dev/null; then
  echo "ERROR: insecure_cookie enabled in profiles.json"
  exit 1
fi

# Verify HTTPS in production profiles
prod_urls=$(jq -r '.profiles[] | select(.network == "main") | .rpc_url' ~/.dinero-cli/profiles.json 2>/dev/null)
for url in $prod_urls; do
  if [[ ! "$url" =~ ^https:// ]]; then
    echo "WARNING: Production profile using non-HTTPS URL: $url"
  fi
done

echo "Security audit complete"
```

## Troubleshooting

### Connection Issues
```bash
# Test with verbose output
dinero-cli --profile prod --verbose --nodeinfo

# Test with retries
dinero-cli --profile prod --retries 5 --timeout 30 --nodeinfo

# Check specific error codes
dinero-cli --profile prod --nodeinfo
echo "Exit code: $?"
# 0=OK, 64=usage, 69=unavailable, 75=temp fail, 77=auth
```

### Paging Issues
```bash
# Test pagination contract
response=$(dinero-cli --profile prod --format json wallet history --limit 2)
echo "$response" | jq '.page'

# Verify page metadata
echo "$response" | jq -e '.page | has("limit") and has("returned") and has("has_more")'
```

### Profile Issues
```bash
# List available profiles
dinero-cli profile list

# Show profile resolution
dinero-cli --profile prod --verbose --nodeinfo 2>&1 | grep "Using profile"

# Test profile override
dinero-cli --profile prod --rpc-url http://backup-node:20998 --nodeinfo
```

## Performance Optimization

### Large Dataset Handling
```bash
# Optimal page size for UTXOs (balance memory vs requests)
dinero-cli --profile prod wallet utxos --limit 1000 --confirmed-only

# Stream large transaction history
offset=0
while true; do
  result=$(dinero-cli --profile prod --format json wallet history --limit 500 --offset $offset)
  
  # Process batch
  echo "$result" | jq -r '.data[] | [.txid, .amount] | @csv'
  
  # Check for more
  has_more=$(echo "$result" | jq -r '.page.has_more // false')
  [ "$has_more" = "true" ] || break
  
  offset=$(echo "$result" | jq -r '.page.next_offset')
done
```

### Monitoring Automation
```bash
#!/bin/bash
# monitor-loop.sh

while true; do
  # Health check
  if ! dinero-cli --profile prod --wait-ready --timeout 10 >/dev/null 2>&1; then
    echo "$(date): Node unhealthy"
    sleep 60
    continue
  fi
  
  # Balance check
  balance=$(dinero-cli --profile prod --format json wallet balance | jq -r '.data.total')
  echo "$(date): Balance: $balance DIN"
  
  # Peer count
  peers=$(dinero-cli --profile prod --format json net peers --limit 1 | jq -r '.page.returned')
  echo "$(date): Connected peers: $peers"
  
  sleep 300  # 5 minutes
done
```

## Emergency Procedures

### Node Unresponsive
```bash
# Quick diagnosis
dinero-cli --profile prod --timeout 5 --retries 1 --nodeinfo || {
  echo "Primary node down, switching to backup"
  dinero-cli --profile prod --rpc-url https://backup-node:20998 --nodeinfo
}
```

### Wallet Issues
```bash
# Test wallet accessibility
dinero-cli --profile prod wallet balance || {
  echo "Wallet access failed"
  # Check if wallet exists
  dinero-cli --profile prod wallet list
}
```

### Data Consistency Check
```bash
# Verify recent transactions
recent_tx=$(dinero-cli --profile prod --format json wallet history --limit 1 | jq -r '.data[0].txid')
dinero-cli --profile prod tx info "$recent_tx" >/dev/null || {
  echo "Transaction data inconsistency detected"
}
```
