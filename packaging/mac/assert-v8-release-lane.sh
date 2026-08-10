#!/usr/bin/env bash
# Refuse to package macOS Qt release assets from stale/retired source lanes.
#
# This guard exists because rc34 was briefly packaged from the old standalone
# dinero-qt checkout even though dinero-v8 already carried the correct Cmd+K
# dashboard. Official macOS GUI artifacts must be built from the dinero-v8
# monorepo root and must carry the dashboard-only Cmd+K surface.

set -euo pipefail

BUILD_DIR=""
APP=""
VERSION=""
EXPECTED_REPO_HEAD=""
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --app) APP="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --expected-repo-head) EXPECTED_REPO_HEAD="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

fail() {
    echo "ERROR: v8 release-lane guard failed: $*" >&2
    exit 1
}

require_file() {
    [[ -f "$1" ]] || fail "missing required file: $1"
}

require_dir() {
    [[ -d "$1" ]] || fail "missing required directory: $1"
}

cache_value() {
    local key="$1"
    local cache="$BUILD_DIR/CMakeCache.txt"
    awk -F= -v k="$key" '
      $1 ~ "^" k ":" { print substr($0, index($0, "=") + 1); found=1; exit }
      END { if (!found) exit 1 }
    ' "$cache"
}

# .git is a directory in a normal clone but a regular file (gitdir pointer) in a
# git worktree; accept either so release builds can run from a worktree.
[[ -e "$PROJECT_ROOT/.git" ]] || fail "missing required git metadata: $PROJECT_ROOT/.git"
require_file "$PROJECT_ROOT/qt/src/mainwindow.cpp"
require_file "$PROJECT_ROOT/qt/src/cmdkpanel.cpp"
require_file "$PROJECT_ROOT/qt/src/mynodedashboard.cpp"
require_file "$PROJECT_ROOT/qt/CMakeLists.txt"

origin_url="$(git -C "$PROJECT_ROOT" remote get-url origin 2>/dev/null || true)"
case "$origin_url" in
    *DineroLabs/dinero-v8*|*github.com:DineroLabs/dinero-v8.git|*github.com/DineroLabs/dinero-v8.git) ;;
    *) fail "origin is not DineroLabs/dinero-v8 (origin=$origin_url)" ;;
esac

repo_head="$(git -C "$PROJECT_ROOT" rev-parse HEAD)"
if [[ -n "$EXPECTED_REPO_HEAD" ]]; then
    expected_head="$(git -C "$PROJECT_ROOT" rev-parse "${EXPECTED_REPO_HEAD}^{commit}" 2>/dev/null || true)"
    [[ -n "$expected_head" ]] || fail "expected source commit does not resolve: $EXPECTED_REPO_HEAD"
else
    expected_head="$repo_head"
fi

grep -q 'kShowAiAssistantPanel = false' "$PROJECT_ROOT/qt/src/mainwindow.cpp" \
    || fail "Cmd+K AI panel is not gated off in qt/src/mainwindow.cpp"
grep -q 'CmdKPanel' "$PROJECT_ROOT/qt/src/mainwindow.cpp" \
    || fail "mainwindow.cpp is not wired to CmdKPanel"
grep -q 'src/mynodedashboard.cpp' "$PROJECT_ROOT/qt/CMakeLists.txt" \
    || fail "qt/CMakeLists.txt does not compile MyNodeDashboard"
grep -q 'src/cmdkpanel.cpp' "$PROJECT_ROOT/qt/CMakeLists.txt" \
    || fail "qt/CMakeLists.txt does not compile CmdKPanel"

if [[ -n "$BUILD_DIR" ]]; then
    require_file "$BUILD_DIR/CMakeCache.txt"
    cmake_home="$(cache_value CMAKE_HOME_DIRECTORY || true)"
    [[ "$cmake_home" == "$PROJECT_ROOT" ]] \
        || fail "build dir was configured from '$cmake_home', expected '$PROJECT_ROOT'"

    cmake_project="$(cache_value CMAKE_PROJECT_NAME || true)"
    [[ "$cmake_project" == "dinero" ]] \
        || fail "build dir project is '$cmake_project', expected monorepo project 'dinero'"

    if [[ -n "$VERSION" ]]; then
        release_tag="$(cache_value DINERO_RELEASE_TAG || true)"
        [[ "$release_tag" == "$VERSION" ]] \
            || fail "DINERO_RELEASE_TAG is '$release_tag', expected '$VERSION'"
    fi
fi

if [[ -n "$APP" ]]; then
    require_dir "$APP"
    identity="$APP/Contents/Resources/release-identity.json"
    exe="$APP/Contents/MacOS/dinero-qt"
    require_file "$identity"
    require_file "$exe"

    python3 - "$identity" "$expected_head" <<'PY'
import json
import sys

path, repo_head = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

qt = data.get("qt", {})
heads = data.get("expected_repo_heads", {})
errors = []
if qt.get("repo") != "dinero-v8":
    errors.append(f"qt.repo={qt.get('repo')!r}, expected 'dinero-v8'")
if qt.get("commit") != repo_head:
    errors.append(f"qt.commit={qt.get('commit')!r}, expected {repo_head}")
if heads.get("dinero") != repo_head:
    errors.append(f"expected_repo_heads.dinero={heads.get('dinero')!r}, expected {repo_head}")
if heads.get("dinero_solo_miner") not in (repo_head, "unknown"):
    errors.append(
        "expected_repo_heads.dinero_solo_miner="
        f"{heads.get('dinero_solo_miner')!r}, expected in-tree {repo_head}"
    )

if errors:
    for error in errors:
        print(error, file=sys.stderr)
    sys.exit(1)
PY

    LC_ALL=C grep -a -q 'dinero::qt::dashboard::CmdKPanel' "$exe" \
        || fail "built dinero-qt binary does not contain CmdKPanel"
    LC_ALL=C grep -a -q 'dinero::qt::dashboard::MyNodeDashboard' "$exe" \
        || fail "built dinero-qt binary does not contain MyNodeDashboard"
fi

echo "OK: v8 release-lane guard passed"
