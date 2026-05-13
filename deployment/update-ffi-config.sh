#!/bin/bash
# Update FFI configuration to use production OpenKYC endpoint
# Run this locally after deploying OpenKYC to production

set -e

PRODUCTION_URL="https://openkyc.dinero-coin.com"
FFI_CONFIG_FILE="../src/wallet/ffi/config.h"

echo "=========================================="
echo "Updating FFI Configuration"
echo "=========================================="
echo ""

# Check if config file exists
if [ ! -f "$FFI_CONFIG_FILE" ]; then
    echo "Creating FFI config file..."
    cat > "$FFI_CONFIG_FILE" << EOF
#pragma once

// OpenKYC Backend Configuration
#define OPENKYC_API_URL "$PRODUCTION_URL"
#define OPENKYC_TIMEOUT_MS 30000  // 30 seconds

// API Endpoints
#define OPENKYC_VERIFY_START_ENDPOINT "/api/verify/start"
#define OPENKYC_VERIFY_STATUS_ENDPOINT "/api/verify/status"
EOF
    echo "✓ Created $FFI_CONFIG_FILE"
else
    # Update existing config
    if grep -q "OPENKYC_API_URL" "$FFI_CONFIG_FILE"; then
        # Replace existing URL
        sed -i.bak "s|#define OPENKYC_API_URL.*|#define OPENKYC_API_URL \"$PRODUCTION_URL\"|" "$FFI_CONFIG_FILE"
        echo "✓ Updated OPENKYC_API_URL to: $PRODUCTION_URL"
    else
        # Add URL if missing
        echo "#define OPENKYC_API_URL \"$PRODUCTION_URL\"" >> "$FFI_CONFIG_FILE"
        echo "✓ Added OPENKYC_API_URL: $PRODUCTION_URL"
    fi
fi

echo ""
echo "Next steps:"
echo "1. Rebuild the FFI library:"
echo "   cd .. && cmake --build build --target dinero_wallet_ffi"
echo ""
echo "2. Copy the updated library to your mobile wallet project"
echo ""
echo "✓ Configuration updated successfully"
