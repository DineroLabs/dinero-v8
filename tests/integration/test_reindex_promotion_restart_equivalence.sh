#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

export REINDEX_PROMOTION_CRASH=1

exec "${ROOT_DIR}/tests/integration/test_reindex_chainstate_utreexo_equivalence.sh"
