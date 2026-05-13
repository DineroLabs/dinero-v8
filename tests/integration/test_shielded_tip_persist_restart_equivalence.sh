#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export SHIELDED_CRASH_HOOK="after_tip_before_checkpoint"
export SHIELDED_EXPECT_COMMITTED="1"

exec "${ROOT_DIR}/tests/integration/test_shielded_daemon_restart_equivalence.sh"
