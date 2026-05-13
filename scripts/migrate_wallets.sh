#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# DineroCoin Wallet Migration Script
# ═══════════════════════════════════════════════════════════════════════════
# Migrates from shared wallet.db to per-wallet database files
#
# Before:  ~/.dinero/wallet.db (all wallets in one DB)
# After:   ~/.dinero/wallets/wallet_<name>.db (one DB per wallet)
#          ~/.dinero/wallet_registry.db (lightweight registry)
#
# Usage: ./scripts/migrate_wallets.sh
# ═══════════════════════════════════════════════════════════════════════════

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  DineroCoin Wallet Migration${NC}"
echo -e "${BLUE}  Shared DB → Per-Wallet DBs${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo

# Configuration
DINERO_DIR="${DINERO_DIR:-$HOME/.dinero}"
OLD_DB="$DINERO_DIR/wallet.db"
NEW_DIR="$DINERO_DIR/wallets"
REGISTRY_DB="$DINERO_DIR/wallet_registry.db"
SCHEMA_DIR="$(dirname "$0")/../resources/schema"
WALLET_SCHEMA="$SCHEMA_DIR/wallet_schema.sql"
REGISTRY_SCHEMA="$SCHEMA_DIR/wallet_registry_schema.sql"
BACKUP_DIR="$DINERO_DIR/backup_$(date +%Y%m%d_%H%M%S)"

# Check if old wallet.db exists
if [ ! -f "$OLD_DB" ]; then
    echo -e "${YELLOW}⚠️  No existing wallet.db found at: $OLD_DB${NC}"
    echo -e "${YELLOW}   This might be a fresh installation.${NC}"
    echo
    echo -e "${GREEN}✓ Creating new wallet directory structure...${NC}"
    mkdir -p "$NEW_DIR"

    # Create wallet registry
    echo -e "${GREEN}✓ Initializing wallet registry...${NC}"
    sqlite3 "$REGISTRY_DB" < "$REGISTRY_SCHEMA"

    echo -e "${GREEN}✅ Fresh installation initialized successfully!${NC}"
    exit 0
fi

# Backup old database
echo -e "${YELLOW}📦 Creating backup...${NC}"
mkdir -p "$BACKUP_DIR"
cp "$OLD_DB" "$BACKUP_DIR/wallet.db.backup"
echo -e "${GREEN}✓ Backup created: $BACKUP_DIR/wallet.db.backup${NC}"
echo

# Create new directory structure
echo -e "${BLUE}📁 Creating new wallet directory...${NC}"
mkdir -p "$NEW_DIR"
echo

# Create wallet registry database
echo -e "${BLUE}📋 Creating wallet registry...${NC}"
if [ -f "$REGISTRY_DB" ]; then
    echo -e "${YELLOW}⚠️  Registry already exists. Backing up...${NC}"
    cp "$REGISTRY_DB" "$BACKUP_DIR/wallet_registry.db.backup"
fi
sqlite3 "$REGISTRY_DB" < "$REGISTRY_SCHEMA"
echo -e "${GREEN}✓ Registry created${NC}"
echo

# Get list of wallets from old database
echo -e "${BLUE}🔍 Discovering wallets in old database...${NC}"
wallet_list=$(sqlite3 "$OLD_DB" "SELECT id, name FROM wallets;" 2>/dev/null || echo "")

if [ -z "$wallet_list" ]; then
    echo -e "${YELLOW}⚠️  No wallets found in old database${NC}"
    echo -e "${GREEN}✅ Migration complete (no wallets to migrate)${NC}"
    exit 0
fi

# Count wallets
wallet_count=$(echo "$wallet_list" | wc -l | tr -d ' ')
echo -e "${GREEN}✓ Found $wallet_count wallet(s)${NC}"
echo

# Migrate each wallet
counter=0
echo "$wallet_list" | while IFS='|' read -r wallet_id wallet_name; do
    counter=$((counter + 1))
    echo -e "${BLUE}[$counter/$wallet_count] Migrating: ${YELLOW}$wallet_name${NC}"

    # Create per-wallet database filename
    safe_name=$(echo "$wallet_name" | tr ' ' '_' | tr -cd '[:alnum:]_-')
    new_db="$NEW_DIR/wallet_${safe_name}.db"

    # Check if wallet DB already exists
    if [ -f "$new_db" ]; then
        echo -e "  ${YELLOW}⚠️  Wallet DB already exists: $new_db${NC}"
        echo -e "  ${YELLOW}   Backing up and recreating...${NC}"
        cp "$new_db" "$BACKUP_DIR/wallet_${safe_name}.db.backup"
        rm "$new_db"
    fi

    # Create new wallet database from schema
    echo -e "  ${BLUE}→ Creating database: $new_db${NC}"
    sqlite3 "$new_db" < "$WALLET_SCHEMA"

    # Extract wallet metadata
    echo -e "  ${BLUE}→ Extracting wallet metadata...${NC}"
    sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db"
-- Insert wallet_meta
INSERT INTO wallet_meta (id, name, network, created_at)
SELECT 1, '$wallet_name', 'mainnet', created_at
FROM wallets WHERE id = $wallet_id LIMIT 1;
EOF

    # Migrate addresses
    echo -e "  ${BLUE}→ Migrating addresses...${NC}"
    sqlite3 "$OLD_DB" "SELECT * FROM addresses WHERE wallet_id = $wallet_id;" | \
        sqlite3 "$new_db" ".import '|cat -' addresses" 2>/dev/null || \
        sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db"
.mode insert addresses
SELECT id, account, change, idx, address, pubkey, label, type, created_at
FROM addresses WHERE wallet_id = $wallet_id;
EOF

    # Migrate UTXOs
    echo -e "  ${BLUE}→ Migrating UTXOs...${NC}"
    sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db"
.mode insert utxos
SELECT id, txid, vout, amount, address, script_pubkey, height, is_coinbase, is_spent,
       spent_txid, spent_height, confirmations, created_at
FROM utxos WHERE wallet_id = $wallet_id;
EOF

    # Migrate transactions
    echo -e "  ${BLUE}→ Migrating transactions...${NC}"
    sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db"
.mode insert transactions
SELECT t.id, t.txid, t.height, t.confirmations, t.time, t.is_coinbase, t.raw_hex, t.created_at
FROM transactions t
INNER JOIN tx_io io ON t.txid = io.txid
WHERE io.txid IN (SELECT txid FROM utxos WHERE wallet_id = $wallet_id)
GROUP BY t.txid;
EOF

    # Migrate HD seeds
    echo -e "  ${BLUE}→ Migrating HD seeds...${NC}"
    sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db"
.mode insert hd_seeds
SELECT 1 as id, encrypted_seed, salt, coin_type,
       COALESCE(encryption_version, 1) as encryption_version, created_at
FROM hd_seeds WHERE wallet_id = $wallet_id LIMIT 1;
EOF

    # Migrate encryption metadata if exists
    echo -e "  ${BLUE}→ Migrating encryption metadata...${NC}"
    sqlite3 "$OLD_DB" <<EOF | sqlite3 "$new_db" 2>/dev/null || true
.mode insert encryption_metadata
SELECT 1 as id, encrypted, kdf, kdf_iterations, kdf_memory_kb, kdf_parallelism,
       cipher, salt, nonce, created_at, updated_at
FROM encryption_metadata WHERE wallet_id = $wallet_id LIMIT 1;
EOF

    # Update wallet_meta with fingerprint if available
    fingerprint=$(sqlite3 "$OLD_DB" "SELECT hex(master_fingerprint) FROM encryption_metadata WHERE wallet_id = $wallet_id LIMIT 1;" 2>/dev/null || echo "")
    if [ -n "$fingerprint" ]; then
        echo -e "  ${BLUE}→ Setting fingerprint: $fingerprint${NC}"
        sqlite3 "$new_db" "UPDATE wallet_meta SET fingerprint = X'$fingerprint' WHERE id = 1;"
    fi

    # Check if wallet is encrypted
    encrypted=$(sqlite3 "$OLD_DB" "SELECT COALESCE(encrypted, 0) FROM encryption_metadata WHERE wallet_id = $wallet_id LIMIT 1;" 2>/dev/null || echo "0")
    if [ "$encrypted" = "1" ]; then
        echo -e "  ${BLUE}→ Marking wallet as encrypted${NC}"
        sqlite3 "$new_db" "UPDATE wallet_meta SET encrypted = 1 WHERE id = 1;"
    fi

    # Run ANALYZE on new database
    sqlite3 "$new_db" "ANALYZE;"

    # Register wallet in registry
    echo -e "  ${BLUE}→ Registering in wallet registry...${NC}"
    sqlite3 "$REGISTRY_DB" <<EOF
INSERT INTO wallets (name, path, network, encrypted, fingerprint, created_at, last_opened)
VALUES ('$wallet_name', '$new_db', 'mainnet', $encrypted,
        $([ -n "$fingerprint" ] && echo "X'$fingerprint'" || echo "NULL"),
        (SELECT created_at FROM sqlite_temp_master LIMIT 1),
        NULL);
EOF

    echo -e "  ${GREEN}✅ Migrated: $wallet_name${NC}"
    echo
done

# Summary
echo
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}✅ Migration Complete!${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo
echo -e "${GREEN}New Structure:${NC}"
echo -e "  ${BLUE}Registry:${NC}  $REGISTRY_DB"
echo -e "  ${BLUE}Wallets:${NC}   $NEW_DIR/"
echo
echo -e "${YELLOW}Backup Location:${NC}"
echo -e "  $BACKUP_DIR"
echo
echo -e "${YELLOW}⚠️  Important Notes:${NC}"
echo -e "  1. Old wallet.db has been backed up"
echo -e "  2. You can safely delete wallet.db after verifying migration"
echo -e "  3. Restart dinerod to use new wallet structure"
echo
echo -e "${GREEN}Next Steps:${NC}"
echo -e "  1. Test wallet access with: dinerod"
echo -e "  2. Verify balances and transactions"
echo -e "  3. If successful, remove old wallet.db:"
echo -e "     ${BLUE}rm $OLD_DB${NC}"
echo
