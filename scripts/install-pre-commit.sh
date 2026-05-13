#!/usr/bin/env bash
set -euo pipefail
# Installs your repo's pre-commit hook by symlinking scripts/pre-commit-hook.sh

root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/.git/hooks"
ln -sf "../../scripts/pre-commit-hook.sh" "$root/.git/hooks/pre-commit"
echo "✅ pre-commit hook installed -> .git/hooks/pre-commit"
