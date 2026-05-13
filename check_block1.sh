#!/bin/bash

echo "═══════════════════════════════════════════════════════════════════════"
echo "CHECKING MAINNET BLOCK 1 (PREMINE BLOCK)"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Check if daemon is running
if ! pgrep -x "dinerod" > /dev/null; then
    echo "⚠️  Daemon not running. Starting it..."
    echo ""
    echo "Run this first:"
    echo "  ./build/bin/dinerod -datadir=data -rpcport=20998"
    echo ""
    exit 1
fi

# Read cookie
COOKIE_FILE="data/mainnet/.cookie"
if [ ! -f "$COOKIE_FILE" ]; then
    echo "❌ Cookie file not found: $COOKIE_FILE"
    exit 1
fi

COOKIE=$(cat "$COOKIE_FILE")

echo "Querying block 1 from mainnet..."
echo ""

curl --user "dinero:$COOKIE" \
  -s \
  -X POST http://127.0.0.1:20998 \
  -H 'Content-Type: application/json' \
  -d '{"method":"getblock","params":[1, 2],"id":1}' | python3 -m json.tool

echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo "WHAT TO CHECK:"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "1. Look for 'tx' array (transactions)"
echo "2. Find the coinbase transaction (first tx)"
echo "3. Look at 'vout' (outputs)"
echo "4. Check for address: din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94"
echo "5. Verify amount: 2627900 DIN (262790000000000 una)"
echo ""
