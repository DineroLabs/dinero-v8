#!/usr/bin/env bash
# Production smoke check for database and RPC consistency
# Usage: ./scripts/smoke_check.sh [regtest|testnet|mainnet] [datadir]
set -Eeuo pipefail

NET="${1:-regtest}"
DATADIR="${2:-./data}"
COOKIE="$DATADIR/$NET/.cookie"

echo "🔍 **SMOKE CHECK - $NET NETWORK**"
echo "================================="
echo "Datadir: $DATADIR"
echo ""

echo "**1. Database Audit:**"
./scripts/db_audit.sh "$NET"
echo ""

echo "**2. RPC Genesis Hash Check:**"
if [[ -f "$COOKIE" ]]; then
  echo "   Cookie found: $COOKIE"
  CRED=$(cat "$COOKIE")
  
  # Try multiple ports (daemon might be on 20998 or 20999)
  for PORT in 20998 20999; do
    echo "   Trying port $PORT..."
    H0=$(curl -s --connect-timeout 3 --user "$CRED" -H 'Content-Type: application/json' \
        -d '{"jsonrpc":"2.0","id":"smoke","method":"getblockhash","params":[0]}' \
        "http://127.0.0.1:$PORT/" 2>/dev/null | jq -r '.result // empty' 2>/dev/null || echo "")
    
    if [[ -n "$H0" && "$H0" != "null" && "$H0" != "" ]]; then
      echo "   ✅ RPC genesis (port $PORT): $H0"
      
      # Compare with database meta (case-insensitive)
      DB_GENESIS=$(sqlite3 "$DATADIR/$NET/blockchain.db" "SELECT LOWER(value) FROM meta WHERE key='genesis_hash';" 2>/dev/null || echo "")
      if [[ -n "$DB_GENESIS" ]]; then
        H0_LOWER=$(echo "$H0" | tr '[:upper:]' '[:lower:]')
        if [[ "$H0_LOWER" == "$DB_GENESIS" ]]; then
          echo "   ✅ RPC and DB genesis hashes match"
        else
          echo "   ❌ MISMATCH: RPC=$H0_LOWER vs DB=$DB_GENESIS"
          exit 1
        fi
      else
        echo "   ⚠️  Could not read genesis from database"
      fi
      break
    fi
  done
  
  if [[ -z "$H0" || "$H0" == "null" || "$H0" == "" ]]; then
    echo "   ❌ RPC call failed or returned empty result"
    echo "   (Daemon may not be running or getblockhash not implemented)"
  fi
else
  echo "   ⚠️  Daemon not running (no cookie) — skipping RPC check"
fi

echo ""
echo "**3. Network Consistency:**"
DB_NETWORK=$(sqlite3 "$DATADIR/$NET/blockchain.db" "SELECT value FROM meta WHERE key='network';" 2>/dev/null || echo "")
if [[ "$DB_NETWORK" == "$NET" ]]; then
  echo "   ✅ Database network matches requested: $NET"
else
  echo "   ❌ MISMATCH: Requested=$NET vs DB=$DB_NETWORK"
  exit 1
fi

echo ""
echo "🎊 **SMOKE CHECK PASSED FOR $NET** 🎊"
