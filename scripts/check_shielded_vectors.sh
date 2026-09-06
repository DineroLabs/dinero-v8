#!/usr/bin/env bash
#
# Cross-build determinism gate for the shielded state commitment.
#
# Consensus digests that differ by toolchain or optimisation level would fork
# the network along build lines. A normal test suite cannot see that class of
# bug: it runs one build, and that build agrees with itself.
#
# This builds the vector dumper under GCC and Clang, in Debug and Release, and
# requires all four to emit output byte-identical to the committed golden file.
#
#   scripts/check_shielded_vectors.sh [--update]
#
# --update rewrites the golden file. Only legitimate when the encoding changed
# ON PURPOSE, and such a change must bump SHIELDED_ROOT_VERSION -- a silent
# golden-file update is exactly the failure this gate exists to catch.
#
# ONE RECORDED EXCEPTION, 2026-09-06 (consensus review finding 7a). Rejecting
# tree roots that are not exactly 32 bytes changed this file WITHOUT a version
# bump, and that is deliberate:
#
#   * Every input the encoder still ACCEPTS produces byte-identical output to
#     before. No digest that any node could have computed from real state has
#     changed -- CommitmentTree::Root() is always 32 bytes, so the production
#     path never reached the removed branch.
#   * What changed is the accepted DOMAIN, not the encoding of accepted values.
#     Bumping the version would invalidate correct v2 digests to describe a
#     change that does not affect any of them.
#   * The entries that moved are six synthetic cases that fed a non-32-byte
#     root and now read REJECTED. Four of them -- tree_len_1, _31, _33, _64 --
#     previously shared ONE digest, d4414d66..., because the encoder
#     zero-filled them: four distinct roots committing to the same value. That
#     collision is the defect, and this file was pinning it.
#
# THE PREMISE THIS RESTS ON, stated so it can be re-checked or disproved:
# NOTHING ON A LIVE PATH HAS EVER COMPUTED A ROOT FROM A NON-32-BYTE TREE ROOT.
# That is not an audit result, it is a type-level property:
#
#     consensus/shielded/commitment_tree.h:37   constexpr size_t HASH_BYTES = 32
#     consensus/shielded/commitment_tree.h:39   using Hash = std::array<uint8_t, HASH_BYTES>
#     consensus/shielded/commitment_tree.h:106  Hash Root() const
#
# Root() returns a fixed 32-byte array BY TYPE, and the sole production caller
# of ComputeShieldedRootFromParts is ComputeShieldedRoot() in
# shielded_root.cpp, which builds its vector from that array. A shorter or
# longer root was unreachable outside tests and the vector dumper, so the
# removed branch never produced a digest any node could have published.
#
# IF THAT PREMISE IS EVER FALSIFIED -- if a shipped caller is found that fed
# arbitrary bytes through the old zero-fill -- THIS DECISION INVERTS and the
# version must be bumped, because deployed nodes would then have published
# digests the new code will not reproduce.
#
# The rule above still stands for any change to the bytes of an ACCEPTED input.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLDEN="${ROOT}/tests/vectors/shielded_state_commitment_v1.txt"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

have() { command -v "$1" >/dev/null 2>&1; }

built=0
declare -a OUTPUTS=()
for cc in g++ clang++; do
    have "$cc" || { echo "skip: $cc not installed"; continue; }
    case "$cc" in
        g++)     c_cc=gcc  ;;
        clang++) c_cc=clang;;
    esac
    for type in Debug Release; do
        label="${cc%%+*}_${type,,}"
        dir="${WORK}/b_${label}"
        echo "==> building ${label}"
        CC="$c_cc" CXX="$cc" cmake -S "${ROOT}" -B "${dir}" \
            -DCMAKE_BUILD_TYPE="${type}" > "${WORK}/${label}.cfg.log" 2>&1
        cmake --build "${dir}" --target dump_shielded_vectors -j "$(nproc 2>/dev/null || echo 4)" \
            > "${WORK}/${label}.build.log" 2>&1
        "${dir}/dump_shielded_vectors" > "${WORK}/${label}.txt"
        OUTPUTS+=("${WORK}/${label}.txt")
        built=$((built + 1))
    done
done

if (( built == 0 )); then
    echo "ERROR: no compiler available; the gate did not run" >&2
    exit 2
fi
if (( built < 4 )); then
    echo "WARNING: only ${built}/4 configurations built; this is a partial gate" >&2
fi

if (( UPDATE )); then
    cp "${OUTPUTS[0]}" "${GOLDEN}"
    echo "golden file updated -- bump SHIELDED_ROOT_VERSION if the encoding changed"
fi

# ── poison value tripwire ─────────────────────────────────────────────────
#
# d4414d6601c8b1307a7a4258401a11ec07abbdfe5c866648b0c714d8677fcb63 is the
# digest the encoder produced for EVERY non-32-byte tree root while it
# zero-filled them: tree_len_1, _31, _33 and _64 all carried it, four distinct
# roots committing to one value. It is the fingerprint of a collision that is
# now rejected, so it must never appear as expected output again.
#
# Deliberately checked in BOTH modes, and against the golden file as well as
# the fresh builds. --update is exactly when this could come back silently: a
# regeneration against a tree where the zero-fill had been reintroduced would
# rewrite the file and report success. This is the two-line tripwire that
# refuses that.
POISON="d4414d6601c8b1307a7a4258401a11ec07abbdfe5c866648b0c714d8677fcb63"
poisoned=0
for f in "${GOLDEN}" "${OUTPUTS[@]}"; do
    [[ -f "${f}" ]] || continue
    if grep -q "${POISON}" "${f}"; then
        echo "POISON VALUE in $(basename "${f}"):" >&2
        grep -n "${POISON}" "${f}" | head -5 | sed 's/^/    /' >&2
        poisoned=1
    fi
done
if (( poisoned )); then
    cat >&2 <<'POISONED'

REFUSING: the zero-fill collision digest has reappeared as expected output.

Every tree root that is not exactly 32 bytes used to hash to this one value,
so a corrupt root and a genuinely empty tree produced identical commitments.
Its return means either the zero-fill was reintroduced in
BuildShieldedRootPreimage, or a vector row was hand-edited back to the old
expected output. Neither may be committed: see consensus review finding 7a.
POISONED
    exit 3
fi

rc=0
for out in "${OUTPUTS[@]}"; do
    if cmp -s "${out}" "${GOLDEN}"; then
        echo "OK   $(basename "${out}")"
    else
        echo "FAIL $(basename "${out}") differs from the golden vectors:"
        diff "${GOLDEN}" "${out}" | head -20
        rc=1
    fi
done

if (( rc == 0 )); then
    echo "PASS: ${built} builds all byte-identical to $(basename "${GOLDEN}")"
fi
exit "${rc}"
