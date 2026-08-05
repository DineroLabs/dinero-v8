#!/bin/sh
# Entrypoint for the official Dinero node image.
#
# Arms the bundled AssumeUTXO snapshot ONLY on a fresh datadir. The rule and the
# reason are copied from qt/src/main.cpp:105 — "on an existing datadir we must NOT
# pass the snapshot (the node is past it)". --assumeutxo_forward_connect=1 must
# accompany it or the active tip is held at the snapshot base for the whole
# genesis->base replay (PR #393), which surfaces to users as 0 confirmations for hours.
set -eu

DATADIR="${DINERO_DATADIR:-/data}"
SNAPSHOT="/opt/dinero/snapshot.dat"

# Base flags: serve the network (listen), talk RPC inside the container only.
set -- \
    -datadir="$DATADIR" \
    -printtoconsole=1 \
    -listen=1 \
    -port=20999 \
    -rpcbind=0.0.0.0 \
    -rpcport=20998 \
    "$@"

if [ ! -d "$DATADIR/blocks" ] && [ ! -d "$DATADIR/blockchain" ]; then
    if [ -f "$SNAPSHOT" ]; then
        echo "[entrypoint] fresh datadir — fast-syncing from bundled AssumeUTXO snapshot"
        set -- "$@" "--assumeutxo_snapshot=$SNAPSHOT" "--assumeutxo_forward_connect=1"
    else
        echo "[entrypoint] fresh datadir but no bundled snapshot — syncing from genesis"
    fi
else
    echo "[entrypoint] existing datadir — not arming snapshot (node is past it)"
fi

echo "[entrypoint] exec dinerod $*"
exec dinerod "$@"
