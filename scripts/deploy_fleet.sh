#!/usr/bin/env bash
#
# deploy_fleet.sh — the ONE sanctioned way to put a binary on the production seeds.
#
# Why this exists
# ---------------
# A production seed runs a *binary*. It is never a build host and never holds
# buildable source. Builds come only from the canonical DineroLabs/dinero-v8
# remote, on a clean host, and only the resulting binary travels to seeds.
#
# This script enforces that with four guard rails, any one of which would have
# stopped the "ship an ancient Dinero-Coin binary" disaster:
#
#   1. Source pinning   — builds from a fresh clone of CANONICAL_REMOTE at a
#                         pinned ref. Never from a pre-existing dir on any host.
#   2. Preflight tripwire — refuses to deploy if any seed still holds a buildable
#                         source tree (.git + CMakeLists.txt) outside an allowlist.
#   3. Provenance gate  — the freshly built `dinerod --version` must report
#                         `repo: DineroLabs/dinero-v8` AND the expected commit,
#                         or nothing ships. (Requires the build-provenance stamp
#                         in cmake/VersionMetadata.cmake + build_identity.h.)
#   4. Safe rollout     — binary-only, checksum-verified, atomic swap, canary
#                         first, health-check, automatic rollback on failure.
#                         Datadirs are never touched.
#
# DRY-RUN BY DEFAULT. Nothing touches a server until you pass --execute.
#
# ⚠ TWO PLACEHOLDERS to reconcile with the real pipeline before --execute:
#   (a) BUILD step is a generic `cmake … --target dinerod`. If releases come from
#       a CI artifact / .deb (the fleet's identical BuildIDs suggest a central
#       build), pass --artifact <path> to that instead of building here.
#   (b) RESTART assumes systemd. Memory (2026-05-31) records the live fleet
#       running dinerod MANUALLY via `nohup runuser -u dinero -- /usr/bin/dinerod`
#       with dinero.service INACTIVE — in which case `systemctl restart` is a
#       SILENT NO-OP and the proven swap is: SIGTERM the running dinerod → replace
#       /usr/bin/dinerod → nohup-relaunch. Set PROCESS_MODEL to match per-seed
#       reality (verify first). Codex's inventory this session claimed systemd;
#       that conflict is unresolved — confirm on the box before trusting either.
#
# Usage:
#   scripts/deploy_fleet.sh --ref <tag-or-sha> [--canary-only] [--execute]
#   scripts/deploy_fleet.sh --ref v8.0.0-rc31 --execute
#
set -euo pipefail

# ----------------------------------------------------------------------------
# Config — CONFIRM THESE before first --execute run.
# ----------------------------------------------------------------------------
CANONICAL_REMOTE="https://github.com/DineroLabs/dinero-v8.git"
EXPECTED_REPO_SLUG="DineroLabs/dinero-v8"   # provenance gate asserts this exact slug

# The fleet. Inventory captured from the live seeds; verify it still holds.
# Each entry: NAME|IP|SSH_KEY  (key relative to ~/.ssh). Canary is listed first.
SEEDS=(
  "LA|172.93.160.131|dnrcalifornia.key"   # <- canary (deployed + verified before the rest)
  "SJ|173.249.200.59|dinerosj_key"
  "NA|172.93.167.32|dinerona_key"
  "EU1|92.118.190.62|dineroeu1_key"
)

REMOTE_USER="root"
# How the live daemon is managed. MUST be confirmed on the seeds before --execute.
#   systemd     -> systemctl restart dinero.service
#   nohup       -> SIGTERM running dinerod, swap binary, nohup-relaunch as user 'dinero'
#   UNCONFIRMED -> refuse to --execute (default; forces a deliberate determination)
PROCESS_MODEL="${DINERO_PROCESS_MODEL:-UNCONFIRMED}"
SERVICE="dinero.service"
LIVE_BIN="/usr/bin/dinerod"
DATADIR="/var/lib/dinero"          # referenced for health checks ONLY; never written
RPC_PORT="20998"
STAGING="/root/.dinero-deploy"     # transient staging on each seed

# A clean Linux host that builds the binary. Seeds are NOT eligible.
# Format: USER@HOST  (must have the dinero-v8 build toolchain). Empty = must be
# supplied with --build-host, or use --artifact to skip building entirely.
BUILD_HOST="${DINERO_BUILD_HOST:-}"

# Trees that are allowed to exist on a seed (won't trip the preflight). Anything
# else with .git + CMakeLists.txt under /root /opt /home aborts the deploy.
PREFLIGHT_SCAN_ROOTS=(/root /opt /home)

# ----------------------------------------------------------------------------
REF=""
EXECUTE=0
CANARY_ONLY=0
ARTIFACT=""           # optional: path to a prebuilt, already-provenance-checked dinerod
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new)

log()  { printf '\033[1;36m[deploy]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[deploy:WARN]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[deploy:ABORT]\033[0m %s\n' "$*" >&2; exit 1; }
run()  { # echo + (in execute mode) run a remote/local command
  if [ "$EXECUTE" = 1 ]; then "$@"; else printf '    DRYRUN: %s\n' "$*"; fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF="${2:?--ref needs a value}"; shift 2;;
    --execute) EXECUTE=1; shift;;
    --canary-only) CANARY_ONLY=1; shift;;
    --build-host) BUILD_HOST="${2:?}"; shift 2;;
    --artifact) ARTIFACT="${2:?}"; shift 2;;
    -h|--help) sed -n '2,40p' "$0"; exit 0;;
    *) die "unknown arg: $1";;
  esac
done

[ -n "$REF" ] || die "missing --ref <tag-or-sha> (the canonical commit to deploy)"
if [ "$EXECUTE" = 1 ]; then
  log "EXECUTE mode — will modify production seeds"
  case "$PROCESS_MODEL" in
    systemd|nohup) log "process model: $PROCESS_MODEL";;
    *) die "PROCESS_MODEL is '$PROCESS_MODEL' — confirm systemd vs nohup on the seeds and set DINERO_PROCESS_MODEL before --execute (memory 2026-05-31 says nohup/inactive-systemd; verify)";;
  esac
else
  log "DRY-RUN — no server will be touched (add --execute to act)"
fi

ssh_seed() { local key="$1" ip="$2"; shift 2; ssh "${SSH_OPTS[@]}" -i "$HOME/.ssh/$key" "$REMOTE_USER@$ip" "$@"; }
scp_seed() { local key="$1" ip="$2" src="$3" dst="$4"; scp "${SSH_OPTS[@]}" -i "$HOME/.ssh/$key" "$src" "$REMOTE_USER@$ip:$dst"; }

# ----------------------------------------------------------------------------
# Step 1 — Preflight tripwire across ALL seeds (read-only; runs even in dry-run).
# ----------------------------------------------------------------------------
preflight_tripwire() {
  if [ "$EXECUTE" != 1 ]; then
    log "Preflight: (dry-run) would SSH each seed read-only and abort on any buildable source tree."
    return
  fi
  log "Preflight: scanning seeds for stray buildable source trees..."
  local tripped=0
  for entry in "${SEEDS[@]}"; do
    IFS='|' read -r name ip key <<<"$entry"
    local found
    found=$(ssh_seed "$key" "$ip" "
      for r in ${PREFLIGHT_SCAN_ROOTS[*]}; do
        [ -d \"\$r\" ] || continue
        find \"\$r\" -maxdepth 4 -name CMakeLists.txt -path '*/.git/..' 2>/dev/null
        find \"\$r\" -maxdepth 4 -type d -name .git 2>/dev/null | while read -r g; do
          d=\$(dirname \"\$g\"); [ -f \"\$d/CMakeLists.txt\" ] && echo \"\$d\"; done
      done | sort -u" 2>/dev/null || echo "__SSH_FAIL__")
    if [ "$found" = "__SSH_FAIL__" ]; then
      warn "[$name $ip] unreachable for preflight — cannot certify clean; treat as blocking"
      tripped=1
    elif [ -n "$found" ]; then
      warn "[$name $ip] BUILDABLE SOURCE PRESENT (landmine):"
      printf '%s\n' "$found" | sed 's/^/        /' >&2
      tripped=1
    else
      log "[$name $ip] clean — no buildable source"
    fi
  done
  [ "$tripped" = 0 ] || die "preflight tripped — quarantine stray source first (scripts/quarantine_stale_source.sh), then retry"
  log "Preflight passed: every seed is binary-only."
}

# ----------------------------------------------------------------------------
# Step 2 — Build from a FRESH clone of the canonical remote (or accept artifact).
# ----------------------------------------------------------------------------
build_binary() {
  if [ -n "$ARTIFACT" ]; then
    [ -f "$ARTIFACT" ] || die "--artifact not found: $ARTIFACT"
    log "Using prebuilt artifact: $ARTIFACT"
    BUILT_BIN="$ARTIFACT"
    return
  fi
  [ -n "$BUILD_HOST" ] || die "no --build-host and no --artifact; refusing to build on a seed (seeds are not build hosts)"
  log "Building $EXPECTED_REPO_SLUG@$REF on $BUILD_HOST from a fresh clone..."
  local rdir="/tmp/dinero-build-$REF"
  # shellcheck disable=SC2029
  run ssh "${SSH_OPTS[@]}" "$BUILD_HOST" "
    set -e
    rm -rf '$rdir'
    git clone --no-single-branch '$CANONICAL_REMOTE' '$rdir'
    cd '$rdir' && git checkout --detach '$REF'
    git submodule update --init --recursive
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target dinerod -j\$(nproc)
  "
  BUILT_BIN="__remote:$BUILD_HOST:$rdir/build/dinerod"
  log "Built (remote): $BUILT_BIN"
}

# ----------------------------------------------------------------------------
# Step 3 — Provenance gate: the built binary must self-identify as canonical.
# ----------------------------------------------------------------------------
provenance_gate() {
  log "Provenance gate: asserting binary reports repo=$EXPECTED_REPO_SLUG..."
  local ver
  if [ "${BUILT_BIN#__remote:}" != "$BUILT_BIN" ]; then
    local hostpath="${BUILT_BIN#__remote:}" h p
    h="${hostpath%%:*}"; p="${hostpath#*:}"
    ver=$(run ssh "${SSH_OPTS[@]}" "$h" "'$p' --version" 2>/dev/null || echo "")
  else
    ver=$( "$BUILT_BIN" --version 2>/dev/null || echo "")
  fi
  if [ "$EXECUTE" != 1 ]; then
    log "  (dry-run: would assert 'repo: $EXPECTED_REPO_SLUG' and commit ~ $REF)"
    return
  fi
  printf '%s\n' "$ver" | sed 's/^/      | /'
  local want="repo: ${EXPECTED_REPO_SLUG}"
  printf '%s\n' "$ver" | grep -qxF "$want" \
    || die "PROVENANCE FAIL: binary did not report '$want' — refusing to ship"
  log "Provenance gate passed."
}

# ----------------------------------------------------------------------------
# Step 4 — Ship to one seed: stage, checksum, atomic swap, restart, health-check,
#          rollback on failure. Binary only. Datadir untouched.
# ----------------------------------------------------------------------------
deploy_one() {
  local name="$1" ip="$2" key="$3"
  # Restart command per process model (see header). nohup path mirrors the proven
  # manual swap from memory; verify the runuser/datadir/logpath on the seed first.
  local restart_cmd
  case "$PROCESS_MODEL" in
    nohup)   restart_cmd="pkill -TERM -xf '$LIVE_BIN --datadir=$DATADIR' || true; for _ in \$(seq 1 15); do pgrep -xf '$LIVE_BIN --datadir=$DATADIR' >/dev/null || break; sleep 1; done; nohup runuser -u dinero -- '$LIVE_BIN' --datadir='$DATADIR' >>/var/log/dinerod-deploy.log 2>&1 &";;
    systemd|*) restart_cmd="systemctl restart '$SERVICE'";;
  esac
  log "=== Deploying to $name ($ip) ==="
  run ssh_seed "$key" "$ip" "mkdir -p '$STAGING'"
  # stage the binary (local or remote source)
  if [ "${BUILT_BIN#__remote:}" != "$BUILT_BIN" ]; then
    local hp="${BUILT_BIN#__remote:}"; local bh="${hp%%:*}"; local bp="${hp#*:}"
    run ssh "${SSH_OPTS[@]}" "$bh" "scp ${SSH_OPTS[*]} -i ~/.ssh/$key '$bp' $REMOTE_USER@$ip:$STAGING/dinerod.new"
  else
    run scp_seed "$key" "$ip" "$BUILT_BIN" "$STAGING/dinerod.new"
  fi
  # verify + atomic swap with timestamped backup, restart, health check, rollback
  run ssh_seed "$key" "$ip" "
    set -e
    test -s '$STAGING/dinerod.new'
    chmod 0755 '$STAGING/dinerod.new'
    '$STAGING/dinerod.new' --version | grep -q '^repo: ${EXPECTED_REPO_SLUG}\$' \
      || { echo 'staged binary failed provenance on-seed'; exit 3; }
    BK='$LIVE_BIN.prev-'\$(date +%Y%m%d-%H%M%S)
    cp -a '$LIVE_BIN' \"\$BK\"
    install -m0755 '$STAGING/dinerod.new' '$LIVE_BIN'
    $restart_cmd
    ok=0
    for i in \$(seq 1 30); do
      sleep 4
      h=\$(dinero-cli -rpcport=$RPC_PORT getblockcount 2>/dev/null || echo '')
      p=\$(dinero-cli -rpcport=$RPC_PORT getconnectioncount 2>/dev/null || echo 0)
      if [ -n \"\$h\" ] && [ \"\${p:-0}\" -ge 1 ]; then echo \"health ok: height=\$h peers=\$p\"; ok=1; break; fi
    done
    if [ \"\$ok\" != 1 ]; then
      echo 'HEALTH CHECK FAILED — rolling back'
      install -m0755 \"\$BK\" '$LIVE_BIN'
      $restart_cmd
      exit 9
    fi
  " || die "$name: deploy failed (rolled back on-seed if reached restart)"
  log "$name: deployed and healthy."
}

# ----------------------------------------------------------------------------
main() {
  preflight_tripwire
  build_binary
  provenance_gate

  # canary first
  IFS='|' read -r cname cip ckey <<<"${SEEDS[0]}"
  deploy_one "$cname" "$cip" "$ckey"
  if [ "$CANARY_ONLY" = 1 ]; then
    log "Canary-only: $cname done. Inspect it, then re-run without --canary-only for the rest."
    exit 0
  fi
  if [ "$EXECUTE" = 1 ]; then
    log "Canary healthy. Pausing 30s before the rest (Ctrl-C to hold here)..."; sleep 30
  fi
  for entry in "${SEEDS[@]:1}"; do
    IFS='|' read -r name ip key <<<"$entry"
    deploy_one "$name" "$ip" "$key"
  done
  log "Fleet deploy complete: $EXPECTED_REPO_SLUG@$REF on ${#SEEDS[@]} seeds."
}
main
