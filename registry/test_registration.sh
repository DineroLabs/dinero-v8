#!/bin/bash
# Test script for Dinero Node Registry self-registration

set -e

REGISTRY_URL="${REGISTRY_URL:-http://localhost:8080}"
NODE_IP="${NODE_IP:-127.0.0.1}"
NODE_PORT="${NODE_PORT:-21999}"

echo "=================================="
echo "Dinero Registry Test Client"
echo "=================================="
echo "Registry: $REGISTRY_URL"
echo "Node: $NODE_IP:$NODE_PORT"
echo ""

# Test 1: Check registry status
echo "📊 Test 1: Check registry status"
STATUS=$(curl -s "$REGISTRY_URL/api/status")
echo "$STATUS" | jq '.'
echo ""

# Test 2: View current nodes
echo "📋 Test 2: View current nodes"
NODES=$(curl -s "$REGISTRY_URL/nodes.json")
TOTAL=$(echo "$NODES" | jq '.total_nodes')
echo "Total nodes: $TOTAL"
echo "$NODES" | jq '.nodes[] | {name, ip, latency_ms, uptime_percentage}'
echo ""

# Test 3: Register a new node (Simple method)
echo "🔧 Test 3: Register node (simple)"
REGISTER_PAYLOAD=$(cat <<EOF
{
  "serverinfo_url": "http://$NODE_IP:$NODE_PORT/serverinfo.json"
}
EOF
)

echo "Payload:"
echo "$REGISTER_PAYLOAD" | jq '.'

REGISTER_RESPONSE=$(curl -s -X POST "$REGISTRY_URL/api/register" \
  -H "Content-Type: application/json" \
  -d "$REGISTER_PAYLOAD")

echo "Response:"
echo "$REGISTER_RESPONSE" | jq '.'
echo ""

# Test 4: Register node (with full info)
echo "🔧 Test 4: Register node (full info)"
FULL_PAYLOAD=$(cat <<EOF
{
  "ip": "$NODE_IP",
  "rpc_port": $NODE_PORT,
  "name": "Test Node",
  "network": "regtest"
}
EOF
)

echo "Payload:"
echo "$FULL_PAYLOAD" | jq '.'

FULL_RESPONSE=$(curl -s -X POST "$REGISTRY_URL/api/register" \
  -H "Content-Type: application/json" \
  -d "$FULL_PAYLOAD")

echo "Response:"
echo "$FULL_RESPONSE" | jq '.'
echo ""

# Test 5: View registered nodes
echo "📋 Test 5: View registered nodes"
REGISTERED=$(curl -s "$REGISTRY_URL/api/registered")
echo "$REGISTERED" | jq '.'
echo ""

# Test 6: Check node statistics
echo "📈 Test 6: Check node statistics"
STATS=$(curl -s "$REGISTRY_URL/api/stats")
echo "$STATS" | jq '.'
echo ""

# Test 7: Check history
echo "📜 Test 7: Check history"
HISTORY=$(curl -s "$REGISTRY_URL/api/history")
echo "$HISTORY" | jq '. | last'
echo ""

echo "=================================="
echo "✅ All tests completed"
echo "=================================="
