#!/bin/bash

# Configuration
RPC_HOST="127.0.0.1"
RPC_PORT="20999"
COOKIE_FILE="/tmp/dinero-regtest/regtest/.cookie"

# Function to make RPC calls
RPC() {
    local method="$1"
    local params="$2"
    
    curl -s -X POST \
        -H "Content-Type: application/json" \
        -H "Authorization: Basic $(echo -n '__cookie__:'$(cat $COOKIE_FILE | cut -d: -f2) | base64)" \
        -d "{\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        http://$RPC_HOST:$RPC_PORT/
}

# Export the function
export -f RPC