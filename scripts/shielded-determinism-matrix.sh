#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${SHIELDED_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-determinism}"
mkdir -p "${out_dir}"
jobs="${SHIELDED_JOBS:-4}"

run_variant() {
  local name="$1" type="$2"; shift 2
  local build="${repo_dir}/build-shielded-det-${name}"
  cmake -S "${repo_dir}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE="${type}" \
    -DBUILD_TESTING=ON -DDINERO_BUILD_QT=OFF "$@"
  cmake --build "${build}" --parallel "${jobs}" --target test_v030_vectors \
    test_shielded_derivation test_shielded_circuit shielded_limits_bench
  "${build}/test_v030_vectors" >"${out_dir}/${name}-v030.log"
  "${build}/test_shielded_derivation" >"${out_dir}/${name}-derivation.log"
  "${build}/test_shielded_circuit" >"${out_dir}/${name}-circuit.log"
  "${build}/tools/pq_bench/shielded_limits_bench" >"${out_dir}/${name}-bench.json"
  sed -E 's/ \([0-9]+ ms( total)?\)//g' "${out_dir}/${name}-v030.log" | shasum -a 256 >"${out_dir}/${name}-v030.sha256"
  sed -E 's/ \([0-9]+ ms( total)?\)//g' "${out_dir}/${name}-derivation.log" | shasum -a 256 >"${out_dir}/${name}-derivation.sha256"
  sed -E 's/ \([0-9]+ ms( total)?\)//g' "${out_dir}/${name}-circuit.log" | shasum -a 256 >"${out_dir}/${name}-circuit.sha256"
}

run_variant clang-release Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
run_variant clang-debug Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

for suite in v030 derivation circuit; do
  diff -u "${out_dir}/clang-release-${suite}.sha256" "${out_dir}/clang-debug-${suite}.sha256" \
    | sed -E 's#(clang-release|clang-debug)#variant#g' >/dev/null || {
      printf 'Determinism mismatch: %s\n' "${suite}" >&2; exit 1;
    }
done
printf 'Clang Debug/Release deterministic test traces match. Evidence: %s\n' "${out_dir}"
