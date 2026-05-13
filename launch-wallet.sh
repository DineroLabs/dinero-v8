#!/bin/bash
# Launch Dinero Qt Wallet with correct datadir
# This ensures the wallet can find the .cookie file for RPC authentication

cd "$(dirname "$0")"

DATADIR="$HOME/.dinero"

echo "🚀 Launching Dinero Qt Wallet..."
echo "📂 Data directory: $DATADIR"
echo "🔐 Cookie file: $DATADIR/.cookie"
echo ""

# Check if cookie exists
if [ ! -f "$DATADIR/.cookie" ]; then
  echo "⚠️  Warning: Cookie file not found!"
  echo "   Make sure dinerod is running with: -datadir=$DATADIR"
  echo ""
fi

# Launch GUI with correct datadir
./gui/build/dinero-qt -datadir="$DATADIR" "$@"
