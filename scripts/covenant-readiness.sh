#!/usr/bin/env bash
# Reproducible local assurance gate for the dormant covenant implementation.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"
run_mutations=0

usage() {
    echo "Usage: $0 [--build-dir DIR] [--mutation]"
    echo "  --build-dir DIR  Existing configured CMake build (default: build)"
    echo "  --mutation       Also require the 16/16 consensus mutation score"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            build_dir="$2"
            shift 2
            ;;
        --mutation)
            run_mutations=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    echo "Configured build directory not found: ${build_dir}" >&2
    echo "Run: cmake -S '${repo_root}' -B '${build_dir}'" >&2
    exit 2
fi

targets=(
    test_bip119_ctv_vectors
    test_covenant_activation
    test_covenant_system_lifecycle
    test_ccv_successor_binding
    test_ccv_bounded_state
    test_covenant_fuzz
    test_ccv_reference_model
    test_ccv_adversarial
    test_covenant_profile_wallet
    test_covenant_wallet_recovery
    test_covenant_scriptpath
    benchmark_covenant_validation
)

echo "== Covenant build =="
cmake --build "${build_dir}" --parallel 4 --target "${targets[@]}"

echo "== Covenant architecture boundaries =="
(
    cd "${repo_root}"
    ./scripts/check_covenant_boundaries.sh
    ./scripts/check_covenant_policy.sh
)

echo "== Covenant CTest lane =="
# --no-tests=error prevents a label/registration mistake from looking green.
ctest --test-dir "${build_dir}" \
    -L covenant \
    --output-on-failure \
    --no-tests=error

if [[ ${run_mutations} -eq 1 ]]; then
    echo "== Covenant mutation score =="
    mutation_report="${build_dir}/covenant-mutation-report.json"
    (
        cd "${repo_root}"
        python3 tools/covenant_mutation_harness.py \
            --build-dir "${build_dir}" \
            --json "${mutation_report}"
    )
    echo "Mutation report: ${mutation_report}"
fi

echo "PASS: local covenant assurance gate"
echo "NOTE: this tests the implementation; it does not arm mainnet activation."
