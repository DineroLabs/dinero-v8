#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_dir}/build-shielded-performance}"
evidence_dir="${SHIELDED_PERF_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-performance}"
mkdir -p "${evidence_dir}"

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DDINERO_BUILD_QT=OFF -DENABLE_GRPC=OFF
cmake --build "${build_dir}" --parallel "${SHIELDED_JOBS:-4}" --target \
  shielded_limits_bench shielded_proof_verify_bench
"${build_dir}/tools/pq_bench/shielded_limits_bench" > "${evidence_dir}/limits.json"
"${build_dir}/tools/pq_bench/shielded_proof_verify_bench" > "${evidence_dir}/proof-verification.json"

if [[ -n "${SHIELDED_DENSE_REINDEX_COMMAND:-}" ]]; then
  /bin/sh -c "${SHIELDED_DENSE_REINDEX_COMMAND}" > "${evidence_dir}/dense-reindex.log" 2>&1
else
  printf '%s\n' 'NOT RUN: set SHIELDED_DENSE_REINDEX_COMMAND to a command using an approved dense-chain fixture.' \
    > "${evidence_dir}/dense-reindex.log"
fi

if [[ -n "${SHIELDED_MOBILE_MEMORY_COMMAND:-}" ]]; then
  /usr/bin/time -l /bin/sh -c "${SHIELDED_MOBILE_MEMORY_COMMAND}" \
    > "${evidence_dir}/mobile-prover-memory.log" 2>&1
else
  printf '%s\n' 'NOT RUN: set SHIELDED_MOBILE_MEMORY_COMMAND to the on-device prover command.' \
    > "${evidence_dir}/mobile-prover-memory.log"
fi

printf 'Shielded performance evidence: %s\n' "${evidence_dir}"
