#!/usr/bin/env bash
# Reproducible local assurance gate for the covenant activation candidate.

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
    dinerod
    test_bip119_ctv_vectors
    test_taproot_scriptpath_consensus
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
    test_escape_hatches
    test_covenant_semantic_oracles
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

echo "== Covenant near-limit resource gate =="
benchmark_report="${build_dir}/covenant-resource-report.json"
"${build_dir}/benchmark_covenant_validation" >"${benchmark_report}"
python3 - "${benchmark_report}" <<'PY'
import json
import sys

report_path = sys.argv[1]
with open(report_path, "r", encoding="utf-8") as handle:
    report = json.load(handle)

ctv = max(report["ctv"], key=lambda row: row["tx_bytes"])
sighash = max(report["taproot_sighash"], key=lambda row: row["tx_bytes"])
ccv = max(report["ccv"], key=lambda row: row["outputs"])

checks = {
    "CTV benchmark reaches >=90 KiB": ctv["tx_bytes"] >= 90_000,
    "CTV precomputed near-limit <=10 ms": ctv["precomputed_us_per_tx"] <= 10_000,
    "Taproot precomputed near-limit <=10 ms": sighash["precomputed_us_per_tx"] <= 10_000,
    "CCV 8192-output transition <=5 ms":
        ccv["outputs"] >= 8_192 and ccv["us_per_transition"] <= 5_000,
}
failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("resource gate failed: " + "; ".join(failed))
print(
    "PASS: resource gate: "
    f"CTV={ctv['precomputed_us_per_tx']}us, "
    f"Taproot={sighash['precomputed_us_per_tx']}us, "
    f"CCV={ccv['us_per_transition']}us"
)
PY
echo "Resource report: ${benchmark_report}"

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
echo "NOTE: this tests the activation candidate; it does not deploy fleet binaries."
