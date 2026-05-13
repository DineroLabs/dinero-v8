#!/bin/bash
# Install the one-liner Phase M.0 pre-commit hook

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK_PATH="$REPO_ROOT/.git/hooks/pre-commit"
SOURCE_HOOK="$REPO_ROOT/scripts/pre-commit"

echo "🔒 Installing Phase M.0 Pre-Commit Hook (One-Liner Edition)"
echo "============================================================"
echo ""

# Check if .git directory exists
if [ ! -d "$REPO_ROOT/.git" ]; then
    echo "❌ Error: Not a git repository"
    exit 1
fi

# Backup existing hook if present
if [ -f "$HOOK_PATH" ]; then
    echo "⚠️  Backing up existing hook to pre-commit.backup"
    cp "$HOOK_PATH" "$HOOK_PATH.backup"
fi

# Copy the hook
cp "$SOURCE_HOOK" "$HOOK_PATH"
chmod +x "$HOOK_PATH"

echo "✅ Phase M.0 hook installed: .git/hooks/pre-commit"
echo ""
echo "The one-liner protection:"
echo '  grep -rn "GetHex()" src/consensus src/daemon | grep -E "(==|!=)" && exit 1'
echo ""
echo "If this ever fires → someone tried to smuggle presentation into logic"
echo ""
echo "Test it now:"
echo "  git commit --allow-empty -m 'Test hook'"
echo ""
