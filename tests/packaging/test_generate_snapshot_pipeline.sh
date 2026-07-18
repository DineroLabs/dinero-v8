#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DATADIR="$TMP/datadir"
OUTDIR="$TMP/published"
MOCK_BIN="$TMP/bin"
CALLS="$TMP/cli.calls"
mkdir -p "$DATADIR" "$OUTDIR" "$MOCK_BIN"

cat > "$MOCK_BIN/dinero-cli" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$MOCK_CALLS"

command=""
for arg in "$@"; do
    case "$arg" in
        -*) ;;
        *) command="$arg"; break ;;
    esac
done

case "$command" in
    getblockcount)
        echo 65300
        ;;
    getblockchaininfo)
        echo '{"initialblockdownload":false}'
        ;;
    dumptxoutset)
        snapshot_path="${@: -1}"
        printf 'snapshot-payload' > "$snapshot_path"
        cat <<JSON
{
  "coins_written": 42,
  "bytes_written": 16,
  "base_hash": "0000000000000000000000000000000000000000000000000000000000000042"
}
JSON
        ;;
    *)
        echo "unexpected command: $command" >&2
        exit 2
        ;;
esac
MOCK
chmod +x "$MOCK_BIN/dinero-cli"

cat > "$MOCK_BIN/gpg" <<'MOCK'
#!/usr/bin/env bash
exit 1
MOCK
chmod +x "$MOCK_BIN/gpg"

export MOCK_CALLS="$CALLS"
export DINERO_CLI="$MOCK_BIN/dinero-cli"
export DINERO_DATADIR="$DATADIR"
export SNAPSHOT_OUTDIR="$OUTDIR"
export PATH="$MOCK_BIN:$PATH"

"$ROOT/scripts/assumeutxo/generate_snapshot.sh" mainnet >/dev/null

if grep -v -- "-datadir=$DATADIR" "$CALLS" | grep -q .; then
    echo "snapshot pipeline omitted -datadir on a dinero-cli call" >&2
    cat "$CALLS" >&2
    exit 1
fi

DUMP_CALL="$(grep 'dumptxoutset' "$CALLS")"
if [[ "$DUMP_CALL" != *"$DATADIR/.utxo-snapshot-mainnet-"* ]]; then
    echo "snapshot pipeline did not export inside the daemon datadir" >&2
    echo "$DUMP_CALL" >&2
    exit 1
fi
# Guard against the original PrivateTmp bug (dumping to the bare system /tmp
# root, e.g. "/tmp/utxo-snapshot-mainnet-$$.dat"). The datadir-containment
# assertion above already proves the dump lands inside the daemon datadir; do
# NOT flag a datadir that merely happens to live under /tmp (Linux mktemp -d
# returns /tmp/tmp.XXXX, which is the daemon-visible datadir here, not the
# systemd-isolated /tmp).
DUMP_PATH="${DUMP_CALL##* }"
if [[ "$DUMP_PATH" != "$DATADIR/"* ]]; then
    echo "snapshot pipeline exported outside the daemon datadir" >&2
    echo "$DUMP_CALL" >&2
    exit 1
fi

SNAPSHOT="$(find "$OUTDIR" -maxdepth 1 -name 'utxo-snapshot-mainnet-height-65300-*.dat' -print -quit)"
MANIFEST="${SNAPSHOT}.manifest.json"
[[ -f "$SNAPSHOT" && -f "$MANIFEST" ]]
grep -q '"height": 65300' "$MANIFEST"

echo "generate_snapshot pipeline datadir/PrivateTmp regression: PASS"
