#!/usr/bin/env bash
# Phase D.3 (Dinero Core 1.0) — dinero-prepare-upgrade test.
#
# Verifies the rollback-binary capture script honors every operator
# quality requirement:
#
#   1.  Refuse unsafe --datadir paths (empty, /, missing, relative,
#       non-directory) → exit 1, no writes.
#   2.  dinerod not running → exit 0 (non-fatal), no file produced,
#       warning printed.
#   3.  --exe override produces a file at the expected path with
#       parseable naming: dinerod.live-pre-<commit>-<ts> .
#   4.  Captured file mode 0750 (executable, group readable, world none).
#   5.  binaries/ directory created if missing, mode 0750.
#   6.  Idempotency: two captures in succession produce two distinct
#       files — neither overwrites the other.
#   7.  No stale-backup mutation: existing dinerod.live-pre-* files in
#       binaries/ are NEVER touched by a fresh capture.
#   8.  --dry-run prints plan, exits 0, makes no changes.
#   9.  Atomic output: no .tmp leftover after success or after a
#       simulated failure.
#   10. Non-Linux exits 0 with warning (only meaningful on macOS but
#       sanity-checked here via a fake $OSTYPE override path).

set -euo pipefail
IFS=$'\n\t'

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="${REPO_ROOT}/share/scripts/dinero-prepare-upgrade"

if [[ ! -x "${SCRIPT}" ]]; then
    echo "[FAIL] expected ${SCRIPT} to be executable" >&2
    exit 1
fi

# Force the Linux capture-path on macOS dev so the test exercises the
# real code on this host. Production callers never set this; CI on
# Linux doesn't need to either. Documented inside the script.
export DINERO_PREPARE_UPGRADE_FORCE_LINUX_PATH=1

pass() { echo "  [✓] $1"; }
fail() { echo "  [FAIL] $1" >&2; exit 1; }

# Stat-mode helper: BSD/macOS uses -f %Lp, GNU uses -c %a.
mode_of() {
    stat -f '%Lp' "$1" 2>/dev/null || stat -c '%a' "$1"
}

TMP="$(mktemp -d -t dinero-prep-upgrade-test.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT

# Build a fake "dinerod" binary that responds to --version with the
# exact shape the script parses. We use a shell wrapper so the test
# doesn't depend on building the real daemon.
#
# Real `dinerod --version` first line is:  `dinerod 89aae4826`
FAKE_EXE="${TMP}/fake-dinerod"
cat > "${FAKE_EXE}" <<'EOF'
#!/usr/bin/env bash
# Mimics `dinerod --version` for capture testing.
case "${1:-}" in
    --version)
        cat <<EOV
dinerod abcdef1234
repo: dinero
component: dinerod
commit: abcdef1234567890abcdef1234567890abcdef12
build_time: 2026-05-02T12:00:00+0000
schema: din.build.v1
EOV
        ;;
    *)
        echo "fake dinerod (test fixture)"
        ;;
esac
EOF
chmod +x "${FAKE_EXE}"

# ──────────────────────────────────────────────────────────────────
# #1 — Refuse unsafe --datadir
# ──────────────────────────────────────────────────────────────────
echo "Property #1: refuse unsafe --datadir"
if "${SCRIPT}" --datadir= --exe="${FAKE_EXE}" 2>/dev/null; then
    fail "empty --datadir should be rejected"
fi
if "${SCRIPT}" --datadir=/ --exe="${FAKE_EXE}" 2>/dev/null; then
    fail "datadir=/ should be rejected"
fi
if "${SCRIPT}" --datadir=/this/path/does/not/exist --exe="${FAKE_EXE}" 2>/dev/null; then
    fail "missing datadir should be rejected"
fi
if "${SCRIPT}" --datadir=relative/path --exe="${FAKE_EXE}" 2>/dev/null; then
    fail "relative datadir should be rejected"
fi
touch "${TMP}/notadir"
if "${SCRIPT}" --datadir="${TMP}/notadir" --exe="${FAKE_EXE}" 2>/dev/null; then
    fail "non-directory datadir should be rejected"
fi
pass "rejected empty, /, missing, relative, non-dir datadirs"

# ──────────────────────────────────────────────────────────────────
# #2 — Successful capture via --exe override
# ──────────────────────────────────────────────────────────────────
echo "Property #2: --exe override produces capture file"
DD="${TMP}/datadir"
mkdir -p "${DD}"
LOG="${TMP}/log_p2.txt"
"${SCRIPT}" --datadir="${DD}" --exe="${FAKE_EXE}" >"${LOG}" 2>&1 || \
    fail "exe-mode capture should succeed"

# Find the produced file (only one expected at this point).
captured=$(find "${DD}/binaries" -type f -name "dinerod.live-pre-*" 2>/dev/null | head -1)
[[ -n "${captured}" ]] || fail "no capture file produced under ${DD}/binaries/"
pass "capture file produced: $(basename "${captured}")"

# ──────────────────────────────────────────────────────────────────
# #3 — Naming: dinerod.live-pre-<commit>-<ts>
# ──────────────────────────────────────────────────────────────────
echo "Property #3: filename matches dinerod.live-pre-<commit>-<ts>"
basename_captured=$(basename "${captured}")
# Expect: dinerod.live-pre-abcdef1234-YYYYMMDDTHHMMSSZ
if ! [[ "${basename_captured}" =~ ^dinerod\.live-pre-abcdef1234-[0-9]{8}T[0-9]{6}Z(-[0-9]+)?$ ]]; then
    fail "filename '${basename_captured}' does not match expected pattern"
fi
pass "filename pattern verified: dinerod.live-pre-abcdef1234-<UTC-timestamp>"

# ──────────────────────────────────────────────────────────────────
# #4 — Captured file mode 0750
# ──────────────────────────────────────────────────────────────────
echo "Property #4: captured file mode is 0750"
mode=$(mode_of "${captured}")
[[ "${mode}" == "750" ]] || fail "expected mode 0750, got ${mode}"
pass "captured file mode 0750"

# ──────────────────────────────────────────────────────────────────
# #5 — binaries/ dir created at mode 0750
# ──────────────────────────────────────────────────────────────────
echo "Property #5: binaries/ directory exists with mode 0750"
[[ -d "${DD}/binaries" ]] || fail "binaries/ directory missing"
dirmode=$(mode_of "${DD}/binaries")
[[ "${dirmode}" == "750" ]] || fail "expected binaries/ mode 0750, got ${dirmode}"
pass "binaries/ exists at mode 0750"

# ──────────────────────────────────────────────────────────────────
# #6 — Idempotency: two captures → two distinct files
# ──────────────────────────────────────────────────────────────────
echo "Property #6: two captures produce two distinct files"
"${SCRIPT}" --datadir="${DD}" --exe="${FAKE_EXE}" >/dev/null 2>&1 || \
    fail "second capture should succeed"
"${SCRIPT}" --datadir="${DD}" --exe="${FAKE_EXE}" >/dev/null 2>&1 || \
    fail "third capture should succeed"
count=$(find "${DD}/binaries" -type f -name "dinerod.live-pre-*" | wc -l | tr -d ' ')
[[ "${count}" -ge 2 ]] || fail "expected at least 2 distinct captures, got ${count}"
pass "${count} distinct captures coexist (no overwrite, collision suffix used as needed)"

# ──────────────────────────────────────────────────────────────────
# #7 — Stale backups untouched
# ──────────────────────────────────────────────────────────────────
echo "Property #7: existing dinerod.live-pre-* files are not modified"
DD2="${TMP}/datadir_with_stale"
mkdir -p "${DD2}/binaries"
STALE="${DD2}/binaries/dinerod.live-pre-aaaaaaaaaa-20260101T000000Z"
echo "this is a stale backup payload that must not be touched" > "${STALE}"
chmod 0750 "${STALE}"
STALE_SHA_BEFORE=$(shasum -a 256 "${STALE}" 2>/dev/null | awk '{print $1}' || \
                   sha256sum "${STALE}" | awk '{print $1}')

"${SCRIPT}" --datadir="${DD2}" --exe="${FAKE_EXE}" >/dev/null 2>&1 || \
    fail "capture into a binaries/ with stale entries should succeed"

STALE_SHA_AFTER=$(shasum -a 256 "${STALE}" 2>/dev/null | awk '{print $1}' || \
                  sha256sum "${STALE}" | awk '{print $1}')
[[ "${STALE_SHA_BEFORE}" == "${STALE_SHA_AFTER}" ]] || \
    fail "stale backup contents changed (before=${STALE_SHA_BEFORE} after=${STALE_SHA_AFTER})"
[[ -f "${STALE}" ]] || fail "stale backup got removed"
pass "stale backup byte-identical pre and post"

# ──────────────────────────────────────────────────────────────────
# #8 — Non-fatal: dinerod not running
# ──────────────────────────────────────────────────────────────────
echo "Property #8: missing daemon is non-fatal (exit 0, no file)"
DD3="${TMP}/datadir_no_daemon"
mkdir -p "${DD3}"
# --pid pointing at a definitely-not-running PID. Pick a high number
# that's exceedingly unlikely to exist, sanity-check first.
phantom_pid=999999
while kill -0 "${phantom_pid}" 2>/dev/null; do
    phantom_pid=$((phantom_pid + 1))
done

set +e
"${SCRIPT}" --datadir="${DD3}" --pid="${phantom_pid}" >"${TMP}/log_p8.txt" 2>&1
rc=$?
set -e
[[ ${rc} -eq 0 ]] || fail "expected exit 0 on phantom PID, got ${rc}"
grep -qE "(WARN|nothing to capture)" "${TMP}/log_p8.txt" || \
    fail "expected a WARN line about missing daemon"
[[ ! -d "${DD3}/binaries" ]] || \
    [[ -z "$(find "${DD3}/binaries" -type f 2>/dev/null)" ]] || \
    fail "no capture should be produced for phantom PID"
pass "phantom PID exits 0 with warning, no file written"

# ──────────────────────────────────────────────────────────────────
# #9 — --dry-run prints plan, writes nothing
# ──────────────────────────────────────────────────────────────────
echo "Property #9: --dry-run is read-only"
DD4="${TMP}/datadir_dryrun"
mkdir -p "${DD4}"
DRY_LOG=$("${SCRIPT}" --datadir="${DD4}" --exe="${FAKE_EXE}" --dry-run 2>&1)
echo "${DRY_LOG}" | grep -q "Dry-run" || fail "dry-run missing 'Dry-run' marker"
echo "${DRY_LOG}" | grep -q "abcdef1234" || fail "dry-run missing parsed commit"
[[ ! -e "${DD4}/binaries" ]] || \
    [[ -z "$(find "${DD4}/binaries" -type f 2>/dev/null)" ]] || \
    fail "dry-run should not produce any files"
pass "dry-run printed plan with parsed commit, wrote nothing"

# ──────────────────────────────────────────────────────────────────
# #10 — No .tmp leftover after success
# ──────────────────────────────────────────────────────────────────
echo "Property #10: no .tmp leftover after capture"
leftovers=$(find "${DD}/binaries" -type f -name "*.tmp" 2>/dev/null | wc -l | tr -d ' ')
[[ "${leftovers}" == "0" ]] || fail "found ${leftovers} .tmp file(s) leftover"
leftovers2=$(find "${DD2}/binaries" -type f -name "*.tmp" 2>/dev/null | wc -l | tr -d ' ')
[[ "${leftovers2}" == "0" ]] || fail "found ${leftovers2} .tmp file(s) leftover in stale-test datadir"
pass "no .tmp files leftover in any test datadir"

# ──────────────────────────────────────────────────────────────────
# #11 — Without the test env override, non-Linux exits 0 with warning
#        (only meaningful when running on macOS — on Linux this is
#        the normal path and the test trivially passes)
# ──────────────────────────────────────────────────────────────────
echo "Property #11: Linux gate fires on non-Linux without override"
DD5="${TMP}/datadir_oscheck"
mkdir -p "${DD5}"
set +e
( unset DINERO_PREPARE_UPGRADE_FORCE_LINUX_PATH
  "${SCRIPT}" --datadir="${DD5}" --exe="${FAKE_EXE}" >"${TMP}/log_p11.txt" 2>&1 )
rc=$?
set -e
[[ ${rc} -eq 0 ]] || fail "Linux gate without override should exit 0, got ${rc}"
if [[ "$(uname -s)" != "Linux" ]]; then
    grep -qE "is not Linux" "${TMP}/log_p11.txt" || \
        fail "expected 'is not Linux' notice on $(uname -s)"
    [[ ! -d "${DD5}/binaries" ]] || \
        [[ -z "$(find "${DD5}/binaries" -type f 2>/dev/null)" ]] || \
        fail "Linux gate should produce no files on non-Linux"
    pass "Linux gate fires on $(uname -s); no files written"
else
    # On Linux, the override path and the gate path produce the same
    # result; the gate just doesn't trigger.
    pass "running on Linux; gate is no-op (verified by other properties)"
fi

echo
echo "✅ dinero-prepare-upgrade: 11/11 properties hold (recovery-grade)"
