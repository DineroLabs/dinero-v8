#!/bin/bash
# DineroCoin Git Status Tool
# Shows comprehensive sync status between local and remote
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Detect repo root
ROOT=$(git rev-parse --show-toplevel 2>/dev/null)

if [[ -z "$ROOT" ]]; then
    echo -e "${RED}Not inside a Git repo.${NC}"
    exit 1
fi

cd "$ROOT"

echo -e "${CYAN}════════════════════════════════════════${NC}"
echo -e "${CYAN}   DineroCoin Git Status Dashboard${NC}"
echo -e "${CYAN}════════════════════════════════════════${NC}"

# Current branch
echo -e "\n${BLUE}Branch:${NC}"
BRANCH=$(git branch --show-current)
echo "  $BRANCH"

# Fetch latest remote info (silently)
git fetch --quiet 2>/dev/null || true

# Untracked files
echo -e "\n${BLUE}Untracked files (not in git):${NC}"
UNTRACKED=$(git ls-files --others --exclude-standard)
if [[ -z "$UNTRACKED" ]]; then
    echo -e "  ${GREEN}None${NC}"
else
    echo "$UNTRACKED" | while read line; do echo -e "  ${YELLOW}+ $line${NC}"; done
fi

# Modified but unstaged
echo -e "\n${BLUE}Modified but unstaged:${NC}"
MODIFIED=$(git diff --name-only)
if [[ -z "$MODIFIED" ]]; then
    echo -e "  ${GREEN}None${NC}"
else
    echo "$MODIFIED" | while read line; do echo -e "  ${RED}M $line${NC}"; done
fi

# Staged but uncommitted
echo -e "\n${BLUE}Staged but uncommitted:${NC}"
STAGED=$(git diff --cached --name-only)
if [[ -z "$STAGED" ]]; then
    echo -e "  ${GREEN}None${NC}"
else
    echo "$STAGED" | while read line; do echo -e "  ${YELLOW}S $line${NC}"; done
fi

# Check if remote tracking branch exists
if git rev-parse --verify @{u} >/dev/null 2>&1; then
    LOCAL=$(git rev-parse @)
    REMOTE=$(git rev-parse @{u})
    BASE=$(git merge-base @ @{u})

    echo -e "\n${BLUE}Local vs Remote sync status:${NC}"
    if [ "$LOCAL" = "$REMOTE" ]; then
        echo -e "  ${GREEN}In sync - everything is pushed${NC}"
    elif [ "$LOCAL" = "$BASE" ]; then
        BEHIND=$(git rev-list --count @..@{u})
        echo -e "  ${YELLOW}Behind remote by $BEHIND commit(s) - need to PULL${NC}"
        echo -e "\n${BLUE}Commits on remote not in local:${NC}"
        git log --oneline @..@{u} | while read line; do echo "  $line"; done
    elif [ "$REMOTE" = "$BASE" ]; then
        AHEAD=$(git rev-list --count @{u}..@)
        echo -e "  ${YELLOW}Ahead of remote by $AHEAD commit(s) - need to PUSH${NC}"
        echo -e "\n${BLUE}Local commits not pushed:${NC}"
        git log --oneline @{u}..@ | while read line; do echo "  $line"; done
    else
        AHEAD=$(git rev-list --count @{u}..@)
        BEHIND=$(git rev-list --count @..@{u})
        echo -e "  ${RED}Diverged - $AHEAD ahead, $BEHIND behind - manual review needed${NC}"
    fi
else
    echo -e "\n${BLUE}Local vs Remote sync status:${NC}"
    echo -e "  ${YELLOW}No upstream tracking branch set${NC}"
fi

# Recent commits
echo -e "\n${BLUE}Recent local commits (last 5):${NC}"
git log -5 --oneline --decorate | while read line; do echo "  $line"; done

# Last push info
echo -e "\n${BLUE}Remote URL:${NC}"
REMOTE_URL=$(git remote get-url origin 2>/dev/null || echo "No remote configured")
echo "  $REMOTE_URL"

# Summary statistics
echo -e "\n${CYAN}════════════════════════════════════════${NC}"
echo -e "${BLUE}Summary:${NC}"

UNTRACKED_COUNT=$(echo "$UNTRACKED" | grep -c . || echo "0")
MODIFIED_COUNT=$(echo "$MODIFIED" | grep -c . || echo "0")
STAGED_COUNT=$(echo "$STAGED" | grep -c . || echo "0")

echo -e "  Untracked:  $UNTRACKED_COUNT"
echo -e "  Modified:   $MODIFIED_COUNT"
echo -e "  Staged:     $STAGED_COUNT"

if [[ "$UNTRACKED_COUNT" == "0" && "$MODIFIED_COUNT" == "0" && "$STAGED_COUNT" == "0" ]]; then
    echo -e "\n  ${GREEN}Working directory is clean${NC}"
fi

echo -e "\n${CYAN}════════════════════════════════════════${NC}"
echo "Done."
