#!/usr/bin/env bash
# Canonical dinero-cli wrapper for local daemon instances.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CLI="${DINERO_CLI:-${REPO_ROOT}/dinero-cli}"
DATADIR="${DINERO_DATADIR:-$HOME/.dinero}"

exec "$CLI" -datadir="$DATADIR" "$@"
