#!/usr/bin/env bash
#
# quarantine_stale_source.sh — find and defuse buildable source trees on the
# production seeds (e.g. an ancient /root/Dinero-Coin checkout).
#
# The disaster this prevents: a future deploy that *builds from* a stale on-seed
# checkout and ships an ancient binary over the live DineroLabs/dinero-v8 nodes.
# A seed should hold a binary, never buildable source. This script audits each
# seed for trees containing both a `.git` dir and a `CMakeLists.txt`, and (with
# --execute) renames them to a loud quarantine name and strips their build dirs
# so nothing can be built from them. It never deletes — you delete by hand once
# you've confirmed the catalog.
#
# DRY-RUN BY DEFAULT. Pass --execute to actually rename/strip.
#
# Usage:
#   scripts/quarantine_stale_source.sh            # audit only (read-only)
#   scripts/quarantine_stale_source.sh --execute  # quarantine in place
#
set -euo pipefail

# Same seed inventory as deploy_fleet.sh — keep them in sync.
SEEDS=(
  "LA|172.93.160.131|dnrcalifornia.key"
  "VA|173.249.195.59|dinerova_key"
  "CN|96.9.226.98|dinerocn_key"
)
REMOTE_USER="root"
SCAN_ROOTS=(/root /opt /home /srv)
QUARANTINE_PREFIX="_QUARANTINE_DO_NOT_BUILD_"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new)

EXECUTE=0
[ "${1:-}" = "--execute" ] && EXECUTE=1

log()  { printf '\033[1;36m[quarantine]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[quarantine:WARN]\033[0m %s\n' "$*" >&2; }

[ "$EXECUTE" = 1 ] && log "EXECUTE mode — will rename/strip stale trees on seeds" \
                   || log "AUDIT (read-only) — pass --execute to quarantine"

# Remote routine: list buildable trees; optionally quarantine them.
remote_script() {
cat <<REMOTE
set -e
mode="$1"
roots="${SCAN_ROOTS[*]}"
prefix="$QUARANTINE_PREFIX"
trees=\$(
  for r in \$roots; do
    [ -d "\$r" ] || continue
    find "\$r" -maxdepth 4 -type d -name .git 2>/dev/null | while read -r g; do
      d=\$(dirname "\$g")
      [ -f "\$d/CMakeLists.txt" ] && echo "\$d"
    done
  done | sort -u
)
if [ -z "\$trees" ]; then echo "CLEAN"; exit 0; fi
echo "\$trees" | while read -r d; do
  remote=\$(git -C "\$d" remote get-url origin 2>/dev/null || echo "?")
  head=\$(git -C "\$d" rev-parse --short HEAD 2>/dev/null || echo "?")
  echo "TREE \$d  origin=\$remote  head=\$head"
  if [ "\$mode" = "execute" ]; then
    base=\$(dirname "\$d"); leaf=\$(basename "\$d")
    # strip build dirs first so nothing is buildable even mid-rename
    find "\$d" -maxdepth 2 -type d \( -name build -o -name 'build-*' -o -name 'cmake-build-*' \) \
      -exec rm -rf {} + 2>/dev/null || true
    tgt="\$base/\${prefix}\${leaf}"
    if [ -e "\$tgt" ]; then echo "  SKIP rename (target exists): \$tgt"; else
      mv "\$d" "\$tgt"; echo "  QUARANTINED -> \$tgt"
    fi
  fi
done
REMOTE
}

any_found=0
for entry in "${SEEDS[@]}"; do
  IFS='|' read -r name ip key <<<"$entry"
  log "=== $name ($ip) ==="
  mode=$([ "$EXECUTE" = 1 ] && echo execute || echo audit)
  out=$(ssh "${SSH_OPTS[@]}" -i "$HOME/.ssh/$key" "$REMOTE_USER@$ip" "bash -s" \
        <<<"$(remote_script "$mode")" 2>/dev/null || echo "__SSH_FAIL__")
  if [ "$out" = "__SSH_FAIL__" ]; then warn "[$name] unreachable"; continue; fi
  if [ "$out" = "CLEAN" ]; then log "[$name] clean — no buildable source"; else
    any_found=1; printf '%s\n' "$out" | sed 's/^/    /'
  fi
done

if [ "$any_found" = 1 ] && [ "$EXECUTE" != 1 ]; then
  warn "Landmines found. Re-run with --execute to quarantine, then delete by hand once verified."
fi
log "Done."
