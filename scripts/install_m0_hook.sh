#!/bin/bash
# Install Phase M.0 pre-commit hook
# This prevents violations from being committed

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK_PATH="$REPO_ROOT/.git/hooks/pre-commit"

echo "📦 Installing Phase M.0 Pre-Commit Hook"
echo "========================================"
echo ""

# Check if .git directory exists
if [ ! -d "$REPO_ROOT/.git" ]; then
    echo "❌ Error: Not a git repository"
    echo "   Run this script from within a git repository"
    exit 1
fi

# Backup existing hook if present
if [ -f "$HOOK_PATH" ]; then
    echo "⚠️  Existing pre-commit hook found"
    echo "   Backing up to pre-commit.backup"
    cp "$HOOK_PATH" "$HOOK_PATH.backup"
fi

# Create the hook
cat > "$HOOK_PATH" << 'EOF'
#!/bin/bash
# Phase M.0 Pre-Commit Hook
# Automatically checks for .GetHex() violations before commit

echo ""
echo "🔒 Running Phase M.0 enforcement check..."
echo ""

# Run the M.0 check script
REPO_ROOT="$(git rev-parse --show-toplevel)"
if [ -f "$REPO_ROOT/scripts/check_m0_violations.sh" ]; then
    bash "$REPO_ROOT/scripts/check_m0_violations.sh"
    EXIT_CODE=$?

    if [ $EXIT_CODE -ne 0 ]; then
        echo ""
        echo "❌ Commit rejected: Phase M.0 violations detected"
        echo ""
        echo "To fix:"
        echo "  1. Review the violations above"
        echo "  2. Use uint256 directly instead of .GetHex()"
        echo "  3. Only use .GetHex() for logging/RPC/storage boundaries"
        echo ""
        echo "To bypass (NOT recommended):"
        echo "  git commit --no-verify"
        echo ""
        exit 1
    fi

    echo ""
else
    echo "⚠️  Warning: Phase M.0 check script not found"
    echo "   Skipping enforcement (commit allowed)"
fi

exit 0
EOF

# Make hook executable
chmod +x "$HOOK_PATH"

echo "✅ Pre-commit hook installed successfully"
echo ""
echo "Location: .git/hooks/pre-commit"
echo ""
echo "The hook will now automatically run on every commit and:"
echo "  ✅ Check for string comparisons using .GetHex()"
echo "  ✅ Check for early downgrades (storing hex in variables)"
echo "  ✅ Verify consensus layer purity"
echo ""
echo "If violations are found, the commit will be rejected."
echo ""
echo "To test the hook:"
echo "  ./scripts/check_m0_violations.sh"
echo ""
