#!/usr/bin/env bash
# Test all SSH keys to find working ones for each server

SERVERS=(
    "96.9.226.98:DineroCA"
    "173.249.195.59:DineroVA"
    "172.93.160.131:DineroLA"
)

KEYS=(
    "$HOME/.ssh/dinero_deploy.key"
    "$HOME/.ssh/dinero_final.key"
    "$HOME/.ssh/dinero_key"
    "$HOME/.ssh/dinero_latest.key"
    "$HOME/.ssh/dinero_va.key"
    "$HOME/.ssh/dinerola.key"
    "$HOME/.ssh/server1.key"
    "$HOME/.ssh/server1_new.key"
    "$HOME/.ssh/server2.key"
    "$HOME/.ssh/test_key1.key"
    "$HOME/.ssh/test_key2.key"
    "$HOME/.ssh/test_key3.key"
)

echo "🔍 Finding working SSH keys for each server..."
echo ""

for server_info in "${SERVERS[@]}"; do
    IP=$(echo "$server_info" | cut -d':' -f1)
    NAME=$(echo "$server_info" | cut -d':' -f2)
    
    echo "Testing $NAME ($IP):"
    
    for key in "${KEYS[@]}"; do
        if [ -f "$key" ]; then
            result=$(timeout 3 ssh -i "$key" -o ConnectTimeout=2 -o StrictHostKeyChecking=no -o BatchMode=yes root@$IP "echo SUCCESS" 2>&1)
            if echo "$result" | grep -q "SUCCESS"; then
                echo "  ✅ WORKS: $key"
                break
            fi
        fi
    done
    echo ""
done
