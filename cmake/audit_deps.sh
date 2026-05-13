#!/bin/bash
# audit_deps.sh - Check for Homebrew dependencies

BINARY="$1"
echo "Auditing dependencies for $(basename "$BINARY"):"
otool -L "$BINARY"

if otool -L "$BINARY" | grep -q /opt/homebrew; then
    echo "ERROR: $(basename "$BINARY") has Homebrew dependencies!"
    otool -L "$BINARY" | grep homebrew
    exit 1
else
    echo "SUCCESS: $(basename "$BINARY") is Homebrew-free"
fi
