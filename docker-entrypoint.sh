#!/bin/sh
# Entrypoint for the official Dinero node image.
#
# The bundled AssumeUTXO snapshot is armed UNCONDITIONALLY — on every start, fresh
# datadir or not — and the daemon's own preconditions decide what to do with it:
#
#   * fresh datadir  -> deferred bootstrap arms, imports once headers reach the base
#     (chainstate_service.cpp, "[snapshot] pending")
#   * existing datadir, chain past the base -> "existing datadir (height N > 0) — NOT
#     auto-loading snapshot"; the flag is inert
#   * existing datadir with persisted AssumeUTXO metadata whose consensus UTXO state is
#     NOT yet at the snapshot base (i.e. the container was restarted mid first-run sync)
#     -> the restore path REHYDRATES from this flag. Without it the daemon logs
#     "Persisted AssumeUTXO metadata exists, but ... no assumeutxo_snapshot path is
#     configured" and exits 2 on every subsequent start, bricking the volume.
#   * a different-base snapshot is still rejected loudly by the daemon's belts.
#
# The earlier "arm only on a fresh datadir" rule (from qt/src/main.cpp:105) keyed off
# the presence of /data/blocks, which the daemon creates seconds into startup — minutes
# BEFORE the import. A restart inside that window dropped the flag and hit exactly the
# fatal branch above. Do not reintroduce it.
#
# --assumeutxo_forward_connect=1 must accompany the snapshot or the active tip is held
# at the snapshot base for the whole genesis->base replay (PR #393), which surfaces to
# users as 0 confirmations for hours.
set -eu

DATADIR="${DINERO_DATADIR:-/data}"
SNAPSHOT="${DINERO_SNAPSHOT:-/opt/dinero/mainnet-snapshot.dat}"

# Base flags: serve the network (listen), talk RPC inside the container only.
set -- \
    -datadir="$DATADIR" \
    -printtoconsole=1 \
    -listen=1 \
    -port=20999 \
    -rpcbind=0.0.0.0 \
    -rpcport=20998 \
    "$@"

if [ -f "$SNAPSHOT" ]; then
    echo "[entrypoint] arming bundled AssumeUTXO snapshot ($SNAPSHOT); the daemon decides whether to use it"
    set -- "$@" "--assumeutxo_snapshot=$SNAPSHOT" "--assumeutxo_forward_connect=1"
else
    echo "[entrypoint] no bundled snapshot at $SNAPSHOT — syncing from genesis"
fi

echo "[entrypoint] exec dinerod $*"
exec dinerod "$@"
