#!/usr/bin/env bash
# =============================================================================
# Fair Launch v5 — Datadir Freeze & Recreate
# 2026-04-11
#
# USAGE: Review each section, then run the relevant commands per machine.
#        DO NOT run this as a single script — it contains sections for
#        different machines (Mac, Dell, Servers).
#
# PRINCIPLE: Move old datadirs aside, never delete. Create fresh datadirs.
#            Copy back ONLY config + wallet material.
# =============================================================================

set -euo pipefail
LEGACY_SUFFIX="legacy-2026-04-11"

# =============================================================================
# MAC (local) — datadir: ~/.dinero
# =============================================================================
# Wallet material to preserve:
#   wallets/             (5 wallet DBs)
#   wallet_registry.db   (wallet name registry)
#   hd_wallet/           (HD wallet state)
#   ct_addresses/        (address labels/book)
#
# NOT carried forward:
#   blockchain/          (chaindb + utxo index)
#   blocks/              (raw block storage)
#   headers/             (header chain)
#   ct_output_index/     (CT output global index)
#   keyimages/           (spent key images)
#   banlist.dat, .cookie, .daemon_id, logs, *.bak.*
# =============================================================================

mac_freeze() {
    echo "=== MAC: Freezing ~/.dinero ==="

    # 1. Stop daemon (graceful)
    ./build/dinero-cli stop 2>/dev/null || echo "  Daemon not running (OK)"
    sleep 3

    # 2. Move entire datadir aside
    if [ -d "$HOME/.dinero" ]; then
        mv "$HOME/.dinero" "$HOME/.dinero-${LEGACY_SUFFIX}"
        echo "  Moved ~/.dinero -> ~/.dinero-${LEGACY_SUFFIX}"
    else
        echo "  WARNING: ~/.dinero does not exist"
        return 1
    fi

    # 3. Create fresh datadir with wallet structure
    mkdir -p "$HOME/.dinero/wallets"
    mkdir -p "$HOME/.dinero/hd_wallet"
    echo "  Created fresh ~/.dinero"

    # 4. Copy back wallet material
    local legacy="$HOME/.dinero-${LEGACY_SUFFIX}"

    # Wallet DBs
    if [ -d "${legacy}/wallets" ]; then
        cp -a "${legacy}/wallets/"*.db "$HOME/.dinero/wallets/" 2>/dev/null && \
            echo "  Copied wallets/*.db" || echo "  No wallet DBs found"
    fi

    # Wallet registry
    if [ -f "${legacy}/wallet_registry.db" ]; then
        cp -a "${legacy}/wallet_registry.db" "$HOME/.dinero/"
        echo "  Copied wallet_registry.db"
    fi

    # HD wallet state
    if [ -d "${legacy}/hd_wallet" ]; then
        cp -a "${legacy}/hd_wallet/"* "$HOME/.dinero/hd_wallet/" 2>/dev/null && \
            echo "  Copied hd_wallet/" || echo "  hd_wallet/ empty"
    fi

    # CT address labels (if any)
    if [ -d "${legacy}/ct_addresses" ]; then
        cp -a "${legacy}/ct_addresses" "$HOME/.dinero/"
        echo "  Copied ct_addresses/"
    fi

    echo "  === MAC DONE ==="
    echo ""
    echo "  Legacy data preserved at: ~/.dinero-${LEGACY_SUFFIX}"
    echo "  Fresh datadir ready at:   ~/.dinero"
    echo ""
    echo "  Contents of legacy (for reference):"
    ls -la "$HOME/.dinero-${LEGACY_SUFFIX}/" | head -20
    echo ""
    echo "  Contents of new datadir:"
    find "$HOME/.dinero" -type f | sort
}

# =============================================================================
# DELL TOWER — datadir: /home/tower/.dinero
# Run these commands via SSH or local terminal on Dell
# =============================================================================

dell_freeze() {
    echo "=== DELL: Freezing /home/tower/.dinero ==="
    local DATADIR="/home/tower/.dinero"
    local LEGACY="${DATADIR}-${LEGACY_SUFFIX}"
    local CLI="/root/Dinero-Coin/build/dinero-cli"  # adjust if different on Dell

    # 1. Stop daemon
    ${CLI} stop 2>/dev/null || echo "  Daemon not running (OK)"
    sleep 3

    # 2. Move aside
    if [ -d "${DATADIR}" ]; then
        mv "${DATADIR}" "${LEGACY}"
        echo "  Moved ${DATADIR} -> ${LEGACY}"
    fi

    # 3. Create fresh
    mkdir -p "${DATADIR}/wallets" "${DATADIR}/hd_wallet"

    # 4. Copy back wallet material + config
    [ -f "${LEGACY}/dinero.conf" ] && cp -a "${LEGACY}/dinero.conf" "${DATADIR}/" && echo "  Copied dinero.conf"
    [ -d "${LEGACY}/wallets" ] && cp -a "${LEGACY}/wallets/"*.db "${DATADIR}/wallets/" 2>/dev/null && echo "  Copied wallets"
    [ -f "${LEGACY}/wallet_registry.db" ] && cp -a "${LEGACY}/wallet_registry.db" "${DATADIR}/" && echo "  Copied wallet_registry.db"
    [ -d "${LEGACY}/hd_wallet" ] && cp -a "${LEGACY}/hd_wallet/"* "${DATADIR}/hd_wallet/" 2>/dev/null && echo "  Copied hd_wallet"

    echo "  === DELL DONE ==="
}

# =============================================================================
# SERVERS (LA, VA, MO, CN) — datadir: /root/.dinero
# Run via SSH to each server, or adapt for ops MCP tool
#
# Server IPs:
#   LA: 45.33.18.44
#   VA: 173.249.195.59
#   MO: 72.18.214.120
#   CN: 96.9.226.98
# =============================================================================

server_freeze() {
    local SERVER_IP="$1"
    local SERVER_NAME="$2"
    local DATADIR="/root/.dinero"
    local LEGACY="${DATADIR}-${LEGACY_SUFFIX}"
    local CLI="/root/Dinero-Coin/build/dinero-cli"

    echo "=== ${SERVER_NAME} (${SERVER_IP}): Freezing ${DATADIR} ==="

    # NOTE: Servers are mining-only — no wallet material to preserve.
    # Only dinero.conf (if it exists) needs to be copied back.

    # 1. Stop daemon (SIGTERM, NOT kill -9)
    ssh root@${SERVER_IP} "${CLI} stop 2>/dev/null || echo 'Daemon not running'"
    sleep 5

    # 2. Verify daemon is stopped
    ssh root@${SERVER_IP} "pgrep -x dinerod && echo 'WARNING: still running' || echo 'Stopped OK'"

    # 3. Move old datadir aside
    ssh root@${SERVER_IP} "mv ${DATADIR} ${LEGACY} && echo 'Moved aside' || echo 'No datadir found'"

    # 4. Create fresh datadir
    ssh root@${SERVER_IP} "mkdir -p ${DATADIR}"

    # 5. Copy back config only
    ssh root@${SERVER_IP} "[ -f ${LEGACY}/dinero.conf ] && cp ${LEGACY}/dinero.conf ${DATADIR}/ && echo 'Copied dinero.conf' || echo 'No dinero.conf'"

    echo "  === ${SERVER_NAME} DONE ==="
    echo ""
}

# =============================================================================
# RUN COMMANDS
# Uncomment the section you want to execute:
# =============================================================================

# --- Mac (run locally) ---
# mac_freeze

# --- Dell (run on Dell or via SSH) ---
# dell_freeze

# --- All 4 servers ---
# server_freeze "45.33.18.44"    "LA"
# server_freeze "173.249.195.59" "VA"
# server_freeze "72.18.214.120"  "MO"
# server_freeze "96.9.226.98"    "CN"

# =============================================================================
# VERIFICATION — run after freeze to confirm state
# =============================================================================

verify_mac() {
    echo "=== Verify Mac ==="
    echo "Legacy exists:"
    ls -d "$HOME/.dinero-${LEGACY_SUFFIX}" 2>/dev/null && echo "  YES" || echo "  NO"
    echo "Fresh datadir:"
    find "$HOME/.dinero" -type f | sort
    echo "Old chain data NOT in new dir:"
    [ -d "$HOME/.dinero/blockchain" ] && echo "  FAIL: blockchain/ exists!" || echo "  OK: no blockchain/"
    [ -d "$HOME/.dinero/headers" ] && echo "  FAIL: headers/ exists!" || echo "  OK: no headers/"
    [ -d "$HOME/.dinero/keyimages" ] && echo "  FAIL: keyimages/ exists!" || echo "  OK: no keyimages/"
}

verify_server() {
    local IP="$1"
    local NAME="$2"
    echo "=== Verify ${NAME} (${IP}) ==="
    ssh root@${IP} "ls -d /root/.dinero-${LEGACY_SUFFIX} 2>/dev/null && echo 'Legacy: YES' || echo 'Legacy: NO'"
    ssh root@${IP} "ls /root/.dinero/ 2>/dev/null || echo 'Fresh datadir empty (OK)'"
    ssh root@${IP} "[ -d /root/.dinero/blockchain ] && echo 'FAIL: old chain data!' || echo 'OK: clean'"
}

# --- Verification ---
# verify_mac
# verify_server "45.33.18.44"    "LA"
# verify_server "173.249.195.59" "VA"
# verify_server "72.18.214.120"  "MO"
# verify_server "96.9.226.98"    "CN"
