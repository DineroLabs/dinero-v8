#!/bin/bash
# Auto-generate API documentation from daemon RPC metadata
# Usage: ./scripts/generate_api_docs.sh [output_format]
# Formats: json (default), markdown

set -euo pipefail

OUTPUT_FORMAT="${1:-json}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Check if daemon is running and get connection info
if [[ -f "$HOME/.dinero/nodeinfo.json" ]]; then
    NODEINFO="$HOME/.dinero/nodeinfo.json"
    COOKIE_FILE="$HOME/.dinero/.cookie"
elif [[ -f "$PROJECT_ROOT/node-mainnet/nodeinfo.json" ]]; then
    NODEINFO="$PROJECT_ROOT/node-mainnet/nodeinfo.json"
    COOKIE_FILE="$PROJECT_ROOT/node-mainnet/.cookie"
else
    echo "❌ No running daemon found. Start dinerod first."
    exit 1
fi

if [[ ! -f "$COOKIE_FILE" ]]; then
    echo "❌ Cookie file not found: $COOKIE_FILE"
    exit 1
fi

AUTH=$(cat "$COOKIE_FILE")
RPC_URL=$(jq -r '.rpc.url' "$NODEINFO")

echo "🔍 Discovering API from daemon at $RPC_URL..."

# Get list of all methods
METHODS=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"rpc.listmethods","params":[]}' "$RPC_URL" \
    | jq -r '.result[]?' 2>/dev/null || echo "")

if [[ -z "$METHODS" ]]; then
    echo "❌ Failed to get method list from daemon"
    exit 1
fi

METHOD_COUNT=$(echo "$METHODS" | wc -l | tr -d ' ')
echo "📋 Found $METHOD_COUNT RPC methods"

# Generate documentation
case "$OUTPUT_FORMAT" in
    "json")
        OUTPUT_FILE="$PROJECT_ROOT/api_docs.json"
        echo "📝 Generating JSON documentation..."
        echo '{"methods": [' > "$OUTPUT_FILE"
        
        FIRST=true
        while IFS= read -r method; do
            [[ -z "$method" ]] && continue
            
            if [[ "$FIRST" == "true" ]]; then
                FIRST=false
            else
                echo "," >> "$OUTPUT_FILE"
            fi
            
            echo "  Documenting $method..."
            curl -s --user "$AUTH" -H 'content-type: application/json' \
                --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"rpc.help\",\"params\":[\"$method\"]}" "$RPC_URL" \
                | jq '.result // {"name":"'$method'","error":"No documentation available"}' >> "$OUTPUT_FILE"
        done <<< "$METHODS"
        
        echo ']}' >> "$OUTPUT_FILE"
        ;;
        
    "markdown")
        OUTPUT_FILE="$PROJECT_ROOT/API_REFERENCE.md"
        echo "📝 Generating Markdown documentation..."
        
        cat > "$OUTPUT_FILE" << 'EOF'
# DineroCoin RPC API Reference

*Auto-generated from daemon metadata*

## Overview

This document lists all available RPC methods with their parameters, return values, and usage examples.

EOF
        
        # Group methods by namespace
        for namespace in wallet blockchain mining rpc; do
            NAMESPACE_METHODS=$(echo "$METHODS" | grep "^$namespace\." || true)
            [[ -z "$NAMESPACE_METHODS" ]] && continue
            
            echo "## $namespace Methods" >> "$OUTPUT_FILE"
            echo "" >> "$OUTPUT_FILE"
            
            while IFS= read -r method; do
                [[ -z "$method" ]] && continue
                echo "  Documenting $method..."
                
                HELP_JSON=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
                    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"rpc.help\",\"params\":[\"$method\"]}" "$RPC_URL" \
                    | jq '.result // {}')
                
                METHOD_NAME=$(echo "$HELP_JSON" | jq -r '.name // "'$method'"')
                DESCRIPTION=$(echo "$HELP_JSON" | jq -r '.description // "No description available"')
                
                echo "### \`$METHOD_NAME\`" >> "$OUTPUT_FILE"
                echo "" >> "$OUTPUT_FILE"
                echo "$DESCRIPTION" >> "$OUTPUT_FILE"
                echo "" >> "$OUTPUT_FILE"
                
                # Parameters
                PARAMS=$(echo "$HELP_JSON" | jq '.params // []')
                if [[ "$PARAMS" != "[]" ]]; then
                    echo "**Parameters:**" >> "$OUTPUT_FILE"
                    echo "$PARAMS" | jq -r '.[] | "- `\(.name)` (\(.type)): \(.description)"' >> "$OUTPUT_FILE"
                    echo "" >> "$OUTPUT_FILE"
                fi
                
                # Return value
                RESULT=$(echo "$HELP_JSON" | jq '.result // {}')
                if [[ "$RESULT" != "{}" ]]; then
                    RESULT_TYPE=$(echo "$RESULT" | jq -r '.type // "unknown"')
                    RESULT_DESC=$(echo "$RESULT" | jq -r '.description // ""')
                    echo "**Returns:** $RESULT_TYPE - $RESULT_DESC" >> "$OUTPUT_FILE"
                    echo "" >> "$OUTPUT_FILE"
                fi
                
                echo "---" >> "$OUTPUT_FILE"
                echo "" >> "$OUTPUT_FILE"
            done <<< "$NAMESPACE_METHODS"
        done
        ;;
        
    *)
        echo "❌ Unknown format: $OUTPUT_FORMAT"
        echo "Supported formats: json, markdown"
        exit 1
        ;;
esac

echo "✅ API documentation generated: $OUTPUT_FILE"
echo "📊 Documented $METHOD_COUNT methods"
