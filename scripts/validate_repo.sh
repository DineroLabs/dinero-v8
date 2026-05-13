#!/bin/bash
# DineroCoin Repository Validation Script
# Run daily to ensure repository integrity

RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

REPO_PATH="/Users/haydarevich/Documents/DineroCoin"
cd "$REPO_PATH" || exit 1

echo "🔍 DineroCoin Repository Validation - $(date)"
echo "================================================"

# 1. Check marker file
if [ -f .dinero_project_root ]; then
    echo -e "${GREEN}✓ Canonical marker file present${NC}"
else
    echo -e "${RED}✗ Missing .dinero_project_root marker!${NC}"
    exit 1
fi

# 2. Check git status
if git status &>/dev/null; then
    echo -e "${GREEN}✓ Git repository valid${NC}"
else
    echo -e "${RED}✗ Not a valid git repository!${NC}"
    exit 1
fi

# 3. Check for uncommitted changes
uncommitted=$(git status --porcelain | wc -l)
if [ "$uncommitted" -gt 0 ]; then
    echo -e "${YELLOW}⚠ $uncommitted uncommitted changes${NC}"
fi

# 4. Check current branch
branch=$(git symbolic-ref --short HEAD)
echo "Current branch: $branch"
if [ "$branch" = "main" ]; then
    echo -e "${YELLOW}⚠ On main branch - remember to use dev for work${NC}"
fi

# 5. Check git remote
expected="git@github.com:Trucker2827/Dinero-Coin.git"
actual=$(git remote get-url origin)
if [ "$actual" = "$expected" ]; then
    echo -e "${GREEN}✓ Git remote correct${NC}"
else
    echo -e "${RED}✗ Git remote mismatch!${NC}"
    echo "  Expected: $expected"
    echo "  Actual: $actual"
fi

# 6. Check for duplicate repos
echo ""
echo "Scanning for duplicate repositories..."
duplicates=$(find /Users/haydarevich/Documents -maxdepth 2 -name ".dinero_project_root" 2>/dev/null | wc -l)
if [ "$duplicates" -eq 1 ]; then
    echo -e "${GREEN}✓ No duplicate repositories${NC}"
else
    echo -e "${RED}✗ Found $duplicates repositories with marker file!${NC}"
    find /Users/haydarevich/Documents -maxdepth 2 -name ".dinero_project_root" 2>/dev/null
fi

# 7. Check build artifacts
build_size=$(du -sh build 2>/dev/null | cut -f1)
echo "Build directory size: ${build_size:-N/A}"

echo ""
echo "================================================"
echo -e "${GREEN}Validation complete${NC}"
