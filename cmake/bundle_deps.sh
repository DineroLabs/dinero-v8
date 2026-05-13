#!/bin/bash
# cmake/bundle_deps.sh - Helper script for bundling dependencies

set -e

APP="$1"
BUNDLE_DIR="$2"
HINT_DIRS="$3"
SCRIPT_PATH="$4"

# Brief delay to ensure binary is fully written
sleep 0.1

if [[ -f "$APP" ]]; then
    echo "Bundling dependencies for $(basename "$APP")..."
    cmake -DAPP="$APP" -DBUNDLE_DIR="$BUNDLE_DIR" -DHINT_DIRS="$HINT_DIRS" -P "$SCRIPT_PATH"
    echo "Dependency bundling completed for $(basename "$APP")"
else
    echo "Warning: Binary not found for bundling: $APP"
    exit 1
fi
