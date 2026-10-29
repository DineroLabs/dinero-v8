#!/usr/bin/env bash
# Execute the real relay/direct/punch namespace harness on an isolated Linux host.
# A missing prerequisite or skipped case is a failure of this qualification gate.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "$(uname -s)" != Linux || "$(id -u)" != 0 || $# != 2 ]]; then
    echo 'usage (Linux root): run_relay_reachability_netns.sh DINEROD EVIDENCE_DIR' >&2
    exit 2
fi
binary="$(realpath "$1")"
evidence="$(realpath -m "$2")"
[[ -x "$binary" ]] || { echo 'dinerod is not executable' >&2; exit 2; }
for tool in ip iptables ip6tables curl jq timeout tc tcpdump python3; do command -v "$tool" >/dev/null; done
mkdir -p "$evidence"
git -c "safe.directory=$root" -C "$root" rev-parse HEAD > "$evidence/source-sha.txt"
failures=0
index=0
run_case() {
    local name="$1"
    shift
    index=$((index + 1))
    local case_dir="$evidence/$name" tag rc passes
    mkdir "$case_dir"
    # Avoid touching any namespace or bridge belonging to another test run.
    while :; do
        tag="r$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' | cut -c1-5)"
        if ! ip netns list | awk '{print $1}' | grep -q "_${tag}$" &&
           ! ip link show "dnbr${tag}" >/dev/null 2>&1; then break; fi
    done
    set +e
    timeout --signal=TERM --kill-after=10s 300s env \
        DINERO_RUN_NETNS_NAT=1 DINEROD="$binary" DINERO_NETNS_TAG="$tag" \
        DINERO_NETNS_RUN_DIR="$case_dir/nodes" "$@" \
        bash "$root/tests/integration/test_relay_nat_netns.sh" > "$case_dir/test.log" 2>&1
    rc=$?
    set -e
    passes="$(grep -c '^\[PASS\]' "$case_dir/test.log" || true)"
    printf '%s exit=%s checks=%s\n' "$name" "$rc" "$passes" | tee -a "$evidence/summary.txt"
    cat "$case_dir/test.log"
    if [[ "$rc" != 0 || "$passes" == 0 ]]; then failures=$((failures + 1)); fi
}
run_case DEFAULT
run_case DIRECT_QUIC DIRECT_QUIC=1
run_case DIALBACK DIALBACK=1
run_case COMMUNITY_RELAY COMMUNITY_RELAY=1
run_case TWO_RELAYS TWO_RELAYS=1
run_case PUNCH_V4 PUNCH_V4=1
run_case PUNCH_V6 PUNCH_V6=1
run_case PUNCH_SYMMETRIC PUNCH_SYMMETRIC=1
run_case PUNCH_V4_LOSS PUNCH_V4=1 DINERO_NETNS_NETEM_LOSS=5%
run_case PUNCH_V6_LOSS PUNCH_V6=1 DINERO_NETNS_NETEM_LOSS=5%
run_case SHAPED_RELAY DINERO_NETNS_NETEM_DELAY=25ms DINERO_NETNS_NETEM_JITTER=5ms DINERO_NETNS_NETEM_LOSS=1% DINERO_NETNS_NETEM_RATE=2mbit
printf 'Executed %s scenarios; failures=%s; skips are failures\n' "$index" "$failures" | tee -a "$evidence/summary.txt"
[[ "$index" == 11 && "$failures" == 0 ]]
