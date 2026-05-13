#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export SHIELDED_SECOND_RESTART_INVALIDITY=1

exec "${ROOT_DIR}/tests/integration/test_shielded_reorg_disconnect_restart_equivalence.sh"
