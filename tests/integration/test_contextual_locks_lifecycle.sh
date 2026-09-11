#!/usr/bin/env bash
set -euo pipefail
export TEST_CONTEXTUAL_LOCKS=1
export CONTEXTUAL_LOCKS_HEIGHT=20
exec "$(dirname "$0")/test_covenant_wallet_multinode_lifecycle.sh" "$@"
