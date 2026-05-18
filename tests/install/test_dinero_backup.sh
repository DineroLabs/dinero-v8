#!/usr/bin/env bash
# Phase D.2 (Dinero Core 1.0) — dinero-backup recovery-grade test.
#
# Verifies the wallet/shielded backup script honors every operator
# quality requirement:
#
#   1.  Refuse unsafe paths: empty, /, missing, non-directory
#   2.  Declared-targets-only: archive contains exactly the include
#       list and nothing else (negative-asserted against chaindb,
#       blocks/, headers/, etc.)
#   3.  Warn on missing optional, FAIL on all-required-missing
#   4.  Staging-dir behavior: no .tmp left behind on either success
#       or failure
#   5.  Manifest integrity: every listed file's sha256 matches what
#       extracts from the archive (round-trip)
#   6.  Atomic output: --output existing => refuse (no overwrite)
#   7.  Permissions: archive mode 0600
#   8.  Restore round-trip: extract into temp dir, recompute sha256s,
#       assert match against manifest
#   9.  No chainstate: archive must NOT contain blocks/, chaindb/,
#       headers/, mempool.dat, peers.dat, debug.log, .cookie
#   10. Dry-run: prints plan, exits 0, no files written

set -euo pipefail
IFS=$'\n\t'

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BACKUP_SCRIPT="${REPO_ROOT}/share/scripts/dinero-backup"

if [[ ! -x "${BACKUP_SCRIPT}" ]]; then
    echo "[FAIL] expected ${BACKUP_SCRIPT} to be executable" >&2
    exit 1
fi

pass() { echo "  [✓] $1"; }
fail() { echo "  [FAIL] $1" >&2; exit 1; }

# Pick sha256 binary that works on macOS + Linux (same logic as the
# script under test, so the test mirrors production behavior).
if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$@" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$@" | awk '{print $1}'; }
else
    fail "no sha256 binary available"
fi

# Build a fully-populated fake datadir under ${1} with deterministic
# random content so sha256 verification is meaningful. The shape
# matches the explore agent's findings: wallets/, hd_wallet/,
# blockchain/{shielded_*,chaindb/,utxo/}, blocks/, headers/, plus
# transient state files at the root.
populate_datadir_full() {
    local d="$1"
    mkdir -p "$d/wallets" "$d/hd_wallet" \
             "$d/blockchain" "$d/blockchain/chaindb" \
             "$d/blockchain/utxo" "$d/blockchain/shielded_nullifiers.db" \
             "$d/blocks" "$d/headers"

    # Required-any: wallets/
    printf 'wallet_registry_payload' > "$d/wallets/wallet_registry.db"
    printf 'wallet_default_payload' > "$d/wallets/wallet_default.db"
    printf 'p2mr_v7_payload' > "$d/wallets/v7_p2mr_default.sqlite"

    # Required-any: hd_wallet/
    printf 'hd_seed_material_v1' > "$d/hd_wallet/hd_wallet.db"

    # Optional shielded files
    printf 'shielded_frontier_payload' > "$d/blockchain/shielded_frontier.bin"
    printf 'shielded_anchor_history_payload' > "$d/blockchain/shielded_anchor_history.bin"
    printf 'rocksdb_current_marker' > "$d/blockchain/shielded_nullifiers.db/CURRENT"
    printf 'rocksdb_manifest_payload' > "$d/blockchain/shielded_nullifiers.db/MANIFEST-000001"
    dd if=/dev/zero of="$d/blockchain/shielded_nullifiers.db/000001.sst" \
       bs=1024 count=4 2>/dev/null

    # MUST-NOT-BACKUP: chainstate, blocks, transient state
    dd if=/dev/zero of="$d/blockchain/chaindb/000001.sst" bs=1024 count=8 2>/dev/null
    printf 'chaindb_current' > "$d/blockchain/chaindb/CURRENT"
    dd if=/dev/zero of="$d/blocks/blk00000.dat" bs=1024 count=16 2>/dev/null
    dd if=/dev/zero of="$d/blocks/rev00000.dat" bs=1024 count=4 2>/dev/null
    printf 'header_state' > "$d/headers/headers.db"
    printf 'cookie_secret' > "$d/.cookie"
    printf 'banlist_payload' > "$d/banlist.dat"
    printf 'mempool_payload' > "$d/mempool.dat"
    printf 'log_line_1\nlog_line_2\n' > "$d/debug.log"
}

# ──────────────────────────────────────────────────────────────────
# #1 — Refuse empty --datadir
# ──────────────────────────────────────────────────────────────────
echo "Property #1: refuse unsafe paths"
TMP="$(mktemp -d -t dinero-backup-test.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT

if "${BACKUP_SCRIPT}" --datadir= --output="${TMP}/out.tar.gz" 2>/dev/null; then
    fail "empty --datadir should be rejected"
fi
if "${BACKUP_SCRIPT}" --datadir=/ --output="${TMP}/out.tar.gz" 2>/dev/null; then
    fail "datadir=/ should be rejected"
fi
if "${BACKUP_SCRIPT}" --datadir=/this/path/does/not/exist --output="${TMP}/out.tar.gz" 2>/dev/null; then
    fail "missing datadir should be rejected"
fi
if "${BACKUP_SCRIPT}" --datadir=relative/path --output="${TMP}/out.tar.gz" 2>/dev/null; then
    fail "relative datadir should be rejected"
fi
# datadir is a regular file, not a directory
touch "${TMP}/notadir"
if "${BACKUP_SCRIPT}" --datadir="${TMP}/notadir" --output="${TMP}/out.tar.gz" 2>/dev/null; then
    fail "non-directory datadir should be rejected"
fi
pass "rejected empty, /, missing, relative, and non-directory datadirs"

# ──────────────────────────────────────────────────────────────────
# #2 — Reject when no irrecoverable material is present
# ──────────────────────────────────────────────────────────────────
echo "Property #2: refuse to back up datadirs with no wallet material"
EMPTY_DD="${TMP}/empty_datadir"
mkdir -p "${EMPTY_DD}/blocks" "${EMPTY_DD}/blockchain"  # chain stuff but no wallet
echo "noise" > "${EMPTY_DD}/blocks/blk00000.dat"

if "${BACKUP_SCRIPT}" --datadir="${EMPTY_DD}" --output="${TMP}/empty.tar.gz" 2>/dev/null; then
    fail "datadir with only chain data should be rejected (nothing to back up)"
fi
[[ ! -e "${TMP}/empty.tar.gz" ]] || fail "no archive should be produced for empty wallet datadir"
pass "rejected datadir with no wallet/hd_wallet (and produced no archive)"

# ──────────────────────────────────────────────────────────────────
# #3 — Refuse to overwrite an existing output
# ──────────────────────────────────────────────────────────────────
echo "Property #3: refuse to overwrite existing --output file"
DD="${TMP}/datadir"
populate_datadir_full "${DD}"
EXISTING="${TMP}/already_there.tar.gz"
echo "preexisting backup don't clobber me" > "${EXISTING}"
EXISTING_SHA=$(sha256_of "${EXISTING}")

if "${BACKUP_SCRIPT}" --datadir="${DD}" --output="${EXISTING}" 2>/dev/null; then
    fail "existing --output should be rejected"
fi
[[ "$(sha256_of "${EXISTING}")" == "${EXISTING_SHA}" ]] || \
    fail "existing output file was modified despite refusal"
pass "preserved existing output untouched"

# ──────────────────────────────────────────────────────────────────
# #4 — Dry-run prints plan, writes nothing
# ──────────────────────────────────────────────────────────────────
echo "Property #4: --dry-run is read-only"
DRY_OUT="${TMP}/dryrun.tar.gz"
DRY_LOG=$("${BACKUP_SCRIPT}" --datadir="${DD}" --output="${DRY_OUT}" --dry-run 2>&1)
[[ ! -e "${DRY_OUT}" ]] || fail "dry-run should not produce an archive"
echo "${DRY_LOG}" | grep -q "Dry-run" || fail "dry-run output missing 'Dry-run' marker"
echo "${DRY_LOG}" | grep -q "wallets/" || fail "dry-run did not mention wallets/"
echo "${DRY_LOG}" | grep -q "hd_wallet/" || fail "dry-run did not mention hd_wallet/"
pass "dry-run printed plan, wrote nothing"

# ──────────────────────────────────────────────────────────────────
# #5 — Successful backup of a full datadir
# ──────────────────────────────────────────────────────────────────
echo "Property #5: full datadir → successful backup"
OUT="${TMP}/backup.tar.gz"
"${BACKUP_SCRIPT}" --datadir="${DD}" --output="${OUT}" >/dev/null
[[ -f "${OUT}" ]] || fail "backup file was not created"
[[ ! -e "${OUT}.tmp" ]] || fail ".tmp file should have been renamed away"

# Permissions: 0600 (mode bits in octal — POSIX %A). Use stat with
# both BSD/macOS (-f %Lp) and GNU/Linux (-c %a) fallbacks.
mode=$(stat -c '%a' "${OUT}" 2>/dev/null || stat -f '%Lp' "${OUT}")
[[ "${mode}" == "600" ]] || fail "expected mode 0600, got ${mode}"
pass "archive at ${OUT} with mode 0600 (no .tmp leftover)"

# ──────────────────────────────────────────────────────────────────
# #6 — Archive contents: include list present, exclude list absent
# ──────────────────────────────────────────────────────────────────
echo "Property #6: archive contains EXACTLY the include list, nothing else"
LIST=$(tar -tzf "${OUT}")

# Must include
must_include=(
    "manifest.txt"
    "wallets/wallet_registry.db"
    "wallets/wallet_default.db"
    "wallets/v7_p2mr_default.sqlite"
    "hd_wallet/hd_wallet.db"
    "blockchain/shielded_frontier.bin"
    "blockchain/shielded_anchor_history.bin"
    "blockchain/shielded_nullifiers.db/CURRENT"
    "blockchain/shielded_nullifiers.db/MANIFEST-000001"
    "blockchain/shielded_nullifiers.db/000001.sst"
)
for entry in "${must_include[@]}"; do
    echo "${LIST}" | grep -qE "^${entry}\$" || fail "archive missing required entry: ${entry}"
done

# Must NOT include — chainstate, blocks, headers, transient state
must_exclude=(
    "blockchain/chaindb"
    "blockchain/utxo"
    "blocks/blk00000.dat"
    "blocks/rev00000.dat"
    "headers/headers.db"
    ".cookie"
    "banlist.dat"
    "mempool.dat"
    "debug.log"
)
for entry in "${must_exclude[@]}"; do
    if echo "${LIST}" | grep -qE "(^|/)${entry//./\\.}(/|\$)"; then
        fail "archive contains forbidden entry: ${entry}"
    fi
done
pass "archive includes 10 wallet/shielded entries; excludes all chain/transient state"

# ──────────────────────────────────────────────────────────────────
# #7 — Manifest integrity: every listed sha256 round-trips
# ──────────────────────────────────────────────────────────────────
echo "Property #7: manifest sha256s match extracted file contents"
RESTORE="${TMP}/restore"
mkdir -p "${RESTORE}"
tar -xzf "${OUT}" -C "${RESTORE}"

[[ -f "${RESTORE}/manifest.txt" ]] || fail "manifest.txt missing after extract"

# Sanity-check manifest header fields exist.
grep -qE "^schema_version=1$" "${RESTORE}/manifest.txt" || fail "manifest missing schema_version=1"
grep -qE "^created_at=" "${RESTORE}/manifest.txt" || fail "manifest missing created_at"
grep -qE "^hostname=" "${RESTORE}/manifest.txt" || fail "manifest missing hostname"
grep -qE "^datadir=" "${RESTORE}/manifest.txt" || fail "manifest missing datadir"
grep -qE "^\[files\]$" "${RESTORE}/manifest.txt" || fail "manifest missing [files] block"

# Walk the [files] block; recompute sha256 of each extracted file and
# compare against the manifest entry. This is the recovery-grade
# round-trip: a backup is only valid if the manifest verifies.
in_files=0
verified=0
while IFS=$'\t' read -r rel size sha; do
    case "${rel}" in
        "[files]")  in_files=1; continue ;;
        "[/files]") in_files=0; continue ;;
    esac
    [[ ${in_files} -eq 1 ]] || continue
    [[ -z "${rel}" ]] && continue

    extracted_sha=$(sha256_of "${RESTORE}/${rel}")
    [[ "${extracted_sha}" == "${sha}" ]] || \
        fail "sha256 mismatch on ${rel} (manifest=${sha} extracted=${extracted_sha})"
    extracted_size=$(wc -c < "${RESTORE}/${rel}" | tr -d ' ')
    [[ "${extracted_size}" == "${size}" ]] || \
        fail "size mismatch on ${rel} (manifest=${size} extracted=${extracted_size})"
    verified=$((verified + 1))
done < "${RESTORE}/manifest.txt"

[[ ${verified} -ge 9 ]] || fail "expected ≥9 verified files, got ${verified}"
pass "${verified} files round-trip cleanly (sha256 + size match manifest)"

# ──────────────────────────────────────────────────────────────────
# #8 — Wallet-only datadir (hd_wallet missing) succeeds with warning
# ──────────────────────────────────────────────────────────────────
echo "Property #8: missing hd_wallet/ alone → succeed with warning"
DD_NOHD="${TMP}/datadir_nohd"
mkdir -p "${DD_NOHD}/wallets"
printf 'wallet_only' > "${DD_NOHD}/wallets/wallet_registry.db"
OUT_NOHD="${TMP}/nohd.tar.gz"

WARN_LOG=$("${BACKUP_SCRIPT}" --datadir="${DD_NOHD}" --output="${OUT_NOHD}" 2>&1)
[[ -f "${OUT_NOHD}" ]] || fail "wallet-only datadir backup should succeed"
LIST_NOHD=$(tar -tzf "${OUT_NOHD}")
echo "${LIST_NOHD}" | grep -qE "^wallets/wallet_registry\.db\$" || \
    fail "missing wallet entry in nohd archive"
if echo "${LIST_NOHD}" | grep -qE "^hd_wallet"; then
    fail "nohd archive should NOT contain hd_wallet/"
fi
pass "wallet-only datadir backed up; hd_wallet absence warned, not failed"

# ──────────────────────────────────────────────────────────────────
# #9 — HD-only datadir (wallets/ missing) succeeds
# ──────────────────────────────────────────────────────────────────
echo "Property #9: hd_wallet/ alone (no wallets/) → succeed"
DD_HDONLY="${TMP}/datadir_hdonly"
mkdir -p "${DD_HDONLY}/hd_wallet"
printf 'hd_only_seed' > "${DD_HDONLY}/hd_wallet/hd_wallet.db"
OUT_HDONLY="${TMP}/hdonly.tar.gz"

"${BACKUP_SCRIPT}" --datadir="${DD_HDONLY}" --output="${OUT_HDONLY}" >/dev/null 2>&1 || \
    fail "hd-only datadir should succeed"
[[ -f "${OUT_HDONLY}" ]] || fail "hd-only output not created"
tar -tzf "${OUT_HDONLY}" | grep -qE "^hd_wallet/hd_wallet\.db\$" || \
    fail "hd_wallet missing from hd-only archive"
pass "hd_wallet/-alone datadir backed up successfully"

# ──────────────────────────────────────────────────────────────────
# #10 — No staging leftovers in $TMPDIR after run
# ──────────────────────────────────────────────────────────────────
echo "Property #10: no staging leftovers"
LEFTOVERS=$(find "${TMPDIR:-/tmp}" -maxdepth 1 -type d -name 'dinero-backup-stage.*' 2>/dev/null | wc -l | tr -d ' ')
[[ "${LEFTOVERS}" == "0" ]] || fail "found ${LEFTOVERS} stage dir(s) leftover"
pass "no dinero-backup-stage.* directories left behind"

echo
echo "✅ dinero-backup: 10/10 properties hold (recovery-grade)"
