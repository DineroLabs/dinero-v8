#!/usr/bin/env bash
# Root-hygiene gate (2026-08 cleanup, see PR "cleanup/root-hygiene").
#
# The repository root once held 133 shell scripts, 34 dead C++/Python files,
# and 5 stale CMakeLists.txt variants — including deployment scripts that
# generated rpcbind=0.0.0.0 / rpcallowip=0.0.0.0/0 daemon configs. Operator
# automation now lives in the private dinero-ops repo; this gate keeps the
# root from regrowing.
#
# Anything root-level matching the guarded classes below must be on the
# allowlist. To add a legitimate root file, extend ALLOWED with a comment
# saying what references it.
set -euo pipefail
cd "$(dirname "$0")/../.."

ALLOWED=(
  "build_nodecore_xcframework.sh"   # iOS NodeCore build; cmake/ThirdParty.cmake points users at it
  "dinero-cli.sh"                   # local RPC convenience wrapper; docs/rpc-standards.md
  "docker-entrypoint.sh"            # Dockerfile ENTRYPOINT; .github/workflows/docker-publish.yml
  "run_fuzzing_suite.sh"            # local fuzzer runner; docs/TESTING_INFRASTRUCTURE_V1.1.md
  "run_regression_tests.sh"         # local regression runner; docs/TESTING_INFRASTRUCTURE_V1.1.md
  "version_config.h"                # tracked default for the cmake-generated header
)

fail=0
for f in *.sh *.cpp *.cc *.c *.py *.h *.hpp CMakeLists.txt.*; do
  [ -e "$f" ] || continue
  ok=0
  for a in "${ALLOWED[@]}"; do
    [ "$f" = "$a" ] && ok=1 && break
  done
  if [ "$ok" -eq 0 ]; then
    echo "ROOT HYGIENE VIOLATION: unexpected root-level file '$f'" >&2
    echo "  Scripts/sources belong under scripts/, src/, tests/, or the private dinero-ops repo." >&2
    echo "  If this file is genuinely needed at the root, allowlist it in scripts/ci/check_root_hygiene.sh" >&2
    echo "  with a comment naming what references it." >&2
    fail=1
  fi
done

# The dangerous config pattern must never come back anywhere in the tree:
# an RPC interface bound to all interfaces with an allow-all source filter.
# (This checker excludes itself — the pattern appears here as documentation.)
pattern='rpcallowip=0\.0\.0\.0/0'
hits=$(grep -rEn --include="*.sh" --exclude-dir=.git --exclude="check_root_hygiene.sh" "$pattern" . 2>/dev/null || true)
if [ -n "$hits" ]; then
  echo "ROOT HYGIENE VIOLATION: a script writes rpcallowip=0.0.0.0/0 (allow-all RPC)." >&2
  echo "$hits" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "root hygiene: OK"
