#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export INVALIDITY_RECONSIDER_CRASH=1

exec "${ROOT_DIR}/tests/integration/test_invalidity_crash_restart_equivalence.sh"
