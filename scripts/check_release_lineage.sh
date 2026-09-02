#!/usr/bin/env bash
# Refuse release builds from lines that predate mandatory persisted-state
# readers. A stale snapshot branch can otherwise compile successfully but fail
# to open every datadir that has written the newer shielded history envelope.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
candidate_ref="${1:-HEAD}"

# Introduced the v2 anchor-history persistence envelope, its v1 migration
# reader, and the restart/reorg recovery required for shielded-history wallets.
readonly shielded_anchor_v2_commit="751fb2461c"

cd "$repo_root"

if ! git cat-file -e "${candidate_ref}^{commit}" 2>/dev/null; then
    echo "ERROR: release candidate is not a commit: ${candidate_ref}" >&2
    exit 2
fi

if ! git cat-file -e "${shielded_anchor_v2_commit}^{commit}" 2>/dev/null; then
    echo "ERROR: release history is shallow; cannot prove mandatory shielded lineage." >&2
    echo "Fetch full history before packaging (actions/checkout fetch-depth: 0)." >&2
    exit 2
fi

if ! git merge-base --is-ancestor "$shielded_anchor_v2_commit" "$candidate_ref"; then
    echo "ERROR: ${candidate_ref} predates mandatory shielded datadir compatibility." >&2
    echo "It cannot read the v2 anchor-history envelope written by current releases." >&2
    echo "Rebase or merge the candidate onto a line containing ${shielded_anchor_v2_commit}." >&2
    exit 1
fi

echo "PASS: ${candidate_ref} contains mandatory shielded datadir compatibility (${shielded_anchor_v2_commit})"
