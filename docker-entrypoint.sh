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

# RPC listens on loopback INSIDE the container only. Binding 0.0.0.0 would expose the
# RPC port to every other container on the same Docker network no matter which ports
# are published — `EXPOSE`/`-p` is not a firewall — and this daemon has no
# `rpcallowip` gate (src/daemon/services/rpc_service.cpp reads `rpcbind` literally),
# so anything that can route to the container IP would get an unauthenticated-by-ACL
# RPC endpoint. `docker exec <c> dinero-cli -datadir=/data ...` runs inside the
# container's network namespace and is unaffected.
#
# DINERO_RPCBIND is a deliberate, documented opt-in for operators who front the RPC
# port themselves. Setting it to 0.0.0.0 publishes RPC to the container network;
# only do that behind an authenticated proxy on a private network.
RPCBIND="${DINERO_RPCBIND:-127.0.0.1}"

# Base flags: serve the network (listen), talk RPC inside the container only.
set -- \
    -datadir="$DATADIR" \
    -printtoconsole=1 \
    -listen=1 \
    -port=20999 \
    -rpcbind="$RPCBIND" \
    -rpcport=20998 \
    "$@"

if [ -f "$SNAPSHOT" ]; then
    set -- "$@" "--assumeutxo_snapshot=$SNAPSHOT" "--assumeutxo_forward_connect=1"
fi

# NEVER echo "$@": a user-supplied -rpcpassword=... (or any other secret flag) would be
# written verbatim into `docker logs`, which is readable by anyone who can reach the
# Docker socket and is routinely shipped to log aggregators.
#
# The startup line below is derived from the FINAL argv rather than from re-testing the
# condition above, so it cannot report the snapshot as armed unless the flags are
# genuinely on the command line. The workflow's restart-regression check asserts on it
# (and on the argv dinerod actually receives).
snapshot_armed=no
forward_connect=no
for a in "$@"; do
    case "$a" in
        --assumeutxo_snapshot=*|-assumeutxo_snapshot=*)             snapshot_armed=yes ;;
        --assumeutxo_forward_connect=1|-assumeutxo_forward_connect=1) forward_connect=yes ;;
    esac
done

# NOTE the field name: rpcbind_default reports what THIS script set, which a trailing
# user-supplied -rpcbind= would override. The two assumeutxo fields are derived from the
# final argv and cannot disagree with it; this one deliberately does not claim to be.
echo "[entrypoint] datadir=$DATADIR rpcbind_default=$RPCBIND assumeutxo_snapshot=$snapshot_armed assumeutxo_forward_connect=$forward_connect"
if [ "$snapshot_armed" = no ]; then
    echo "[entrypoint] no snapshot armed (looked for $SNAPSHOT) — the daemon will sync from genesis"
fi
echo "[entrypoint] starting dinerod (arguments not logged — they may contain secrets)"
exec dinerod "$@"
