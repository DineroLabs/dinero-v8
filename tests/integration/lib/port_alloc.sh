#!/usr/bin/env bash
# Shared port allocation for the integration suite.
#
# WHY THIS EXISTS
#
# Scripts used to pick ports with a bare `PORT=$((BASE + RANDOM % N))` and no
# check that anything was already listening. Two ways that bites:
#
#   * the windows are narrow and OVERLAP each other — 44000 + RANDOM % 400 sits
#     inside 36000 + RANDOM % 12000 (36000-48000), so two unrelated tests can
#     draw the same port;
#   * a daemon that has not fully exited from an earlier test still holds its
#     listener, and nothing noticed.
#
# Either way the node fails to bind, the test hangs, then times out far past its
# normal runtime. Observed on CSNShieldedReorgInvertibility: 449s against a
# ~104s norm, with the harness reporting only a connection failure.
#
# USAGE
#
#   source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
#   RPC_PORT=$(alloc_port_base)        # 8 consecutive free ports from RPC_PORT
#   RPC_PORT=$(alloc_port_base 4)      # or ask for a specific run length
#
# Returns the BASE of a run of consecutive free ports, so existing
# `P2P=$((RPC_PORT+1))` derivations keep working unchanged.

# True when something is already LISTENING on $1.
_port_in_use() {
    local port="$1"
    if command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1
        return $?
    fi
    # No lsof (minimal containers, some CI images): fall back to a connect
    # probe via bash's /dev/tcp, which needs no external tool. A successful
    # connect means someone is listening.
    (exec 3<>"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1 && { exec 3<&- 3>&-; return 0; }
    return 1
}

# Echo the base of `count` (default 8) consecutive free ports.
alloc_port_base() {
    local count="${1:-8}"
    local attempt base port busy
    for attempt in $(seq 1 200); do
        # 30000-49999 keeps clear of the ephemeral range on Linux
        # (net.ipv4.ip_local_port_range typically starts at 32768, but the
        # lsof/connect check below is what actually guarantees availability).
        base=$((30000 + RANDOM % 20000))
        busy=0
        for ((port = base; port < base + count; port++)); do
            if _port_in_use "${port}"; then
                busy=1
                break
            fi
        done
        if [[ "${busy}" == "0" ]]; then
            printf '%s\n' "${base}"
            return 0
        fi
    done
    echo "alloc_port_base: no run of ${count} free ports after 200 attempts" >&2
    return 1
}
