#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MODE="dry-run"
FORCE_STOP=0
KEEP_ROLLBACKS=1
ROOT="${REPO_ROOT%/*}"
ACTIVE="${REPO_ROOT}/build/dinerod"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="/tmp/prune_dinerod_binaries-${TIMESTAMP}.log"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Prune stale executable 'dinerod' binaries under a root path.
Default mode is dry-run (no changes).

Options:
  --dry-run                 Preview actions only (default)
  --apply                   Apply changes (create rollback + delete stale binaries)
  --force-stop              Stop running dinerod processes when used with --apply
  --keep-rollbacks N        Keep newest N rollback binaries (default: 1)
  --root PATH               Search root for stale binaries (default: ${ROOT})
  --active PATH             Active binary to keep (default: ${ACTIVE})
  -h, --help                Show this help

Examples:
  $(basename "$0")
  $(basename "$0") --apply --force-stop
  $(basename "$0") --apply --force-stop --keep-rollbacks 2 --root /Users/haydarevich/src
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            MODE="dry-run"
            shift
            ;;
        --apply)
            MODE="apply"
            shift
            ;;
        --force-stop)
            FORCE_STOP=1
            shift
            ;;
        --keep-rollbacks)
            [[ $# -ge 2 ]] || { echo "ERROR: --keep-rollbacks requires a value"; exit 1; }
            KEEP_ROLLBACKS="$2"
            shift 2
            ;;
        --root)
            [[ $# -ge 2 ]] || { echo "ERROR: --root requires a path"; exit 1; }
            ROOT="$2"
            shift 2
            ;;
        --active)
            [[ $# -ge 2 ]] || { echo "ERROR: --active requires a path"; exit 1; }
            ACTIVE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: Unknown argument: $1"
            usage
            exit 1
            ;;
    esac
done

if ! [[ "${KEEP_ROLLBACKS}" =~ ^[0-9]+$ ]]; then
    echo "ERROR: --keep-rollbacks must be a non-negative integer"
    exit 1
fi

ROOT="$(cd "${ROOT}" && pwd)"
ACTIVE_DIR="$(cd "$(dirname "${ACTIVE}")" && pwd)"
ACTIVE="${ACTIVE_DIR}/$(basename "${ACTIVE}")"

exec > >(tee "${LOG_FILE}") 2>&1

echo "== Dinero Binary Prune =="
echo "mode=${MODE}"
echo "root=${ROOT}"
echo "active=${ACTIVE}"
echo "keep_rollbacks=${KEEP_ROLLBACKS}"
echo "force_stop=${FORCE_STOP}"
echo "log_file=${LOG_FILE}"
echo

if [[ ! -f "${ACTIVE}" ]]; then
    echo "ERROR: active binary not found: ${ACTIVE}"
    exit 1
fi

if [[ ! -x "${ACTIVE}" ]]; then
    echo "ERROR: active binary is not executable: ${ACTIVE}"
    exit 1
fi

RUNNING=()
while IFS= read -r line; do
    RUNNING+=("${line}")
done < <(
    ps -axo pid=,command= | awk '
    {
        pid=$1
        $1=""
        sub(/^ /, "", $0)
        cmd=$0
        if (cmd ~ /(^|[[:space:]])([^[:space:]]*\/)?dinerod([[:space:]]|$)/) {
            print pid " " cmd
        }
    }'
)
if [[ "${#RUNNING[@]}" -gt 0 ]]; then
    echo "Detected running dinerod processes:"
    printf '  %s\n' "${RUNNING[@]}"
    echo
    if [[ "${MODE}" == "dry-run" ]]; then
        echo "Dry-run mode: no processes will be stopped."
        echo "Apply mode requires --force-stop while dinerod is running."
        echo
    elif [[ "${MODE}" == "apply" && "${FORCE_STOP}" -eq 1 ]]; then
        echo "Stopping running dinerod processes (--force-stop enabled)..."
        PIDS=()
        while IFS= read -r pid; do
            PIDS+=("${pid}")
        done < <(printf '%s\n' "${RUNNING[@]}" | awk '{print $1}')
        for pid in "${PIDS[@]}"; do
            kill -TERM "${pid}" 2>/dev/null || true
        done
        sleep 1
        for pid in "${PIDS[@]}"; do
            if ps -p "${pid}" >/dev/null 2>&1; then
                kill -KILL "${pid}" 2>/dev/null || true
            fi
        done
        echo "Stopped running dinerod processes."
        echo
    else
        echo "Refusing to continue while dinerod is running."
        echo "Use --apply --force-stop to stop processes and continue."
        exit 2
    fi
fi

CANDIDATES=()
while IFS= read -r -d '' path; do
    CANDIDATES+=("${path}")
done < <(find "${ROOT}" -type f -name dinerod -perm -111 -print0 2>/dev/null || true)

ROLLBACKS=()
while IFS= read -r path; do
    ROLLBACKS+=("${path}")
done < <(find "${ACTIVE_DIR}" -maxdepth 1 -type f -name 'dinerod.rollback-*' -print | sort)

contains_path() {
    local needle="$1"
    shift || true
    local item
    for item in "$@"; do
        if [[ "${item}" == "${needle}" ]]; then
            return 0
        fi
    done
    return 1
}

KEEP_PATHS=("${ACTIVE}")
KEEP_ROLLBACK_PATHS=()

NEW_BACKUP=""
if [[ "${MODE}" == "apply" ]]; then
    NEW_BACKUP="${ACTIVE}.rollback-${TIMESTAMP}"
    echo "Creating rollback copy: ${NEW_BACKUP}"
    cp -p "${ACTIVE}" "${NEW_BACKUP}"
    chmod +x "${NEW_BACKUP}"
    ROLLBACKS+=("${NEW_BACKUP}")
    echo
fi

if [[ "${#ROLLBACKS[@]}" -gt 0 ]]; then
    SORTED_ROLLBACKS=()
    while IFS= read -r path; do
        [[ -n "${path}" ]] || continue
        SORTED_ROLLBACKS+=("${path}")
    done < <(printf '%s\n' "${ROLLBACKS[@]}" | sort)
    ROLLBACKS=("${SORTED_ROLLBACKS[@]}")
fi

if [[ "${#ROLLBACKS[@]}" -gt 0 && "${KEEP_ROLLBACKS}" -gt 0 ]]; then
    start_index=$(( ${#ROLLBACKS[@]} - KEEP_ROLLBACKS ))
    if [[ "${start_index}" -lt 0 ]]; then
        start_index=0
    fi
    for (( i=start_index; i<${#ROLLBACKS[@]}; i++ )); do
        KEEP_ROLLBACK_PATHS+=("${ROLLBACKS[i]}")
    done
fi

for path in "${KEEP_ROLLBACK_PATHS[@]}"; do
    KEEP_PATHS+=("${path}")
done

ROLLBACK_DELETE_LIST=()
for path in "${ROLLBACKS[@]}"; do
    if ! contains_path "${path}" "${KEEP_ROLLBACK_PATHS[@]}"; then
        ROLLBACK_DELETE_LIST+=("${path}")
    fi
done

BIN_DELETE_LIST=()
for path in "${CANDIDATES[@]}"; do
    [[ -n "${path}" ]] || continue
    if ! contains_path "${path}" "${KEEP_PATHS[@]}"; then
        BIN_DELETE_LIST+=("${path}")
    fi
done

echo "Found executable dinerod binaries: ${#CANDIDATES[@]}"
echo "Keeping:"
for k in "${KEEP_PATHS[@]}"; do
    echo "  ${k}"
done | sort
echo

if [[ "${#BIN_DELETE_LIST[@]}" -gt 0 ]]; then
    if [[ "${MODE}" == "dry-run" ]]; then
        echo "Would delete stale dinerod binaries (${#BIN_DELETE_LIST[@]}):"
    else
        echo "Deleting stale dinerod binaries (${#BIN_DELETE_LIST[@]}):"
    fi
    printf '  %s\n' "${BIN_DELETE_LIST[@]}"
    echo
fi

if [[ "${#ROLLBACK_DELETE_LIST[@]}" -gt 0 ]]; then
    if [[ "${MODE}" == "dry-run" ]]; then
        echo "Would delete old rollbacks (${#ROLLBACK_DELETE_LIST[@]}):"
    else
        echo "Deleting old rollbacks (${#ROLLBACK_DELETE_LIST[@]}):"
    fi
    printf '  %s\n' "${ROLLBACK_DELETE_LIST[@]}"
    echo
fi

if [[ "${#BIN_DELETE_LIST[@]}" -eq 0 && "${#ROLLBACK_DELETE_LIST[@]}" -eq 0 ]]; then
    echo "Nothing to delete."
fi

if [[ "${MODE}" == "apply" && "${#BIN_DELETE_LIST[@]}" -gt 0 ]]; then
    for path in "${BIN_DELETE_LIST[@]}"; do
        rm -f "${path}"
    done
fi

if [[ "${MODE}" == "apply" && "${#ROLLBACK_DELETE_LIST[@]}" -gt 0 ]]; then
    for path in "${ROLLBACK_DELETE_LIST[@]}"; do
        rm -f "${path}"
    done
fi

REMAINING=()
while IFS= read -r -d '' path; do
    REMAINING+=("${path}")
done < <(find "${ROOT}" -type f -name dinerod -perm -111 -print0 2>/dev/null || true)
echo "Remaining executable dinerod binaries: ${#REMAINING[@]}"
printf '  %s\n' "${REMAINING[@]}" | sed '/^$/d' | sort
echo

echo "Active binary version:"
"${ACTIVE}" --version | head -n 3

echo
echo "Done. Full report: ${LOG_FILE}"
