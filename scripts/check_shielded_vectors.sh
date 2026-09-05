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
