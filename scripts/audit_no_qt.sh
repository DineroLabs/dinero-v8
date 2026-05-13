#!/usr/bin/env bash
set -euo pipefail
# Fail if Qt types appear outside GUI
grep -RIn --include='*.h' --include='*.cpp' '\bQ(string|Map|Hash|Vector|List|ByteArray)\b' \
  src include | grep -v '^src/gui' && { echo "Qt leaked into core/daemon"; exit 1; } || exit 0