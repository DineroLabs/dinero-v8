#!/usr/bin/env bash
# The watchtower is a shipped program (macOS/Linux): `cmake --install` of its
# component must place a runnable dinero-swap-tower (and the door of a public
# tower, dinero-swap-tower-intake) in <prefix>/bin.
#   usage: swap_tower_install_check.sh <cmake> <build-dir>
set -euo pipefail
CMAKE=${1:?cmake}; BUILD=${2:?build dir}
PREFIX=$(mktemp -d)
trap 'rm -rf "$PREFIX"' EXIT
"$CMAKE" --install "$BUILD" --component swap-tower --prefix "$PREFIX" >/dev/null
BIN="$PREFIX/bin/dinero-swap-tower"
[ -x "$BIN" ] || { echo "FAIL: $BIN not installed"; ls -R "$PREFIX"; exit 1; }
set +e
OUT=$("$BIN" 2>&1); CODE=$?
set -e
[ "$CODE" = 2 ] && grep -q "missing --inbox" <<<"$OUT" || { echo "FAIL: installed tower: exit $CODE: $OUT"; exit 1; }
# With it comes the door of a public tower, and nothing else.
DOOR="$PREFIX/bin/dinero-swap-tower-intake"
set +e
OUT=$("$DOOR" 2>&1); CODE=$?
set -e
[ "$CODE" = 2 ] && grep -q -- "--inbox DIR --listen HOST:PORT" <<<"$OUT" || { echo "FAIL: installed intake: exit $CODE: $OUT"; exit 1; }
[ "$(ls "$PREFIX/bin" | tr '\n' ' ')" = "dinero-swap-tower dinero-swap-tower-intake " ] || { echo "FAIL: component installs something else: $(ls "$PREFIX/bin")"; exit 1; }
echo "OK: installed $BIN runs (usage, exit 2)"
