#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${SHIELDED_JOBS:-4}"
duration="${SHIELDED_FUZZ_SECONDS:-60}"
evidence_dir="${SHIELDED_SECURITY_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-security}"
security_cc="${SHIELDED_SECURITY_CC:-clang}"
security_cxx="${SHIELDED_SECURITY_CXX:-clang++}"
mkdir -p "${evidence_dir}"
failed=0

# CMake's ENABLE_SANITIZERS interface is opt-in per target. It did not reach
# these tests or their libraries. Explicit flags cover every source compiled in
# these dedicated builds; the prebuilt OpenSSL library remains uninstrumented.
configure_run() {
  local name="$1" sanitizers="$2"; shift 2
  local dir="${repo_dir}/build-shielded-${name}"
  local flags="-fsanitize=${sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all"
  cmake -S "${repo_dir}" -B "${dir}" -G Ninja -DBUILD_TESTING=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDINERO_BUILD_QT=OFF \
    -DCMAKE_C_COMPILER="${security_cc}" -DCMAKE_CXX_COMPILER="${security_cxx}" \
    -DCMAKE_C_FLAGS="${flags}" -DCMAKE_CXX_FLAGS="${flags}" "$@"
  python3 "${repo_dir}/scripts/ci/check_shielded_sanitizer_coverage.py" \
    "${dir}" "${sanitizers}" > "${evidence_dir}/${name}-coverage.json"
  cmake --build "${dir}" --parallel "${jobs}" --target test_anchor_history \
    test_shielded_serialization test_shielded_derivation test_spartan_soundness
  ctest --test-dir "${dir}" --output-on-failure --no-tests=error \
    --output-junit "${evidence_dir}/${name}-junit.xml" -R \
    '^(AnchorHistory|ShieldedSerialization|ShieldedDerivation|SpartanSoundness)$'
}

fuzz_run() {
  local fuzz_dir="${repo_dir}/build-shielded-fuzz-llvm"
  local fuzz_cc="${SHIELDED_FUZZ_CC:-clang}"
  local fuzz_cxx="${SHIELDED_FUZZ_CXX:-clang++}"
  local runtime_sanitizers="address,undefined"
  if [[ "$(uname -s)" == Darwin ]] && command -v brew >/dev/null 2>&1; then
    local llvm_prefix
    llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
    if [[ -x "${llvm_prefix}/bin/clang++" ]]; then
      fuzz_cc="${llvm_prefix}/bin/clang"
      fuzz_cxx="${llvm_prefix}/bin/clang++"
      runtime_sanitizers="undefined"
    fi
  fi
  local flags="-fsanitize=${runtime_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all"
  cmake -S "${repo_dir}" -B "${fuzz_dir}" -G Ninja -DBUILD_TESTING=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDINERO_BUILD_QT=OFF -DENABLE_FUZZING=ON \
    -DDINERO_FUZZ_SANITIZERS="fuzzer,${runtime_sanitizers}" \
    -DCMAKE_C_FLAGS="${flags}" -DCMAKE_CXX_FLAGS="${flags}" \
    -DCMAKE_C_COMPILER="${fuzz_cc}" -DCMAKE_CXX_COMPILER="${fuzz_cxx}"
  python3 "${repo_dir}/scripts/ci/check_shielded_sanitizer_coverage.py" \
    "${fuzz_dir}" "${runtime_sanitizers}" --fuzz > "${evidence_dir}/fuzz-coverage.json"
  cmake --build "${fuzz_dir}" --parallel "${jobs}" --target fuzz_shielded_surfaces fuzz_compact_spartan
  "${fuzz_dir}/fuzz/fuzz_shielded_surfaces" "${fuzz_dir}/fuzz_corpus/shielded" \
    -artifact_prefix="${evidence_dir}/shielded-crash-" \
    -max_len=200000 -timeout=5 -max_total_time="${duration}" -print_final_stats=1
  "${fuzz_dir}/fuzz/fuzz_compact_spartan" "${fuzz_dir}/fuzz_corpus/compact_spartan" \
    -artifact_prefix="${evidence_dir}/compact-crash-" \
    -max_len=65536 -timeout=5 -max_total_time="${duration}" -print_final_stats=1
}

# Retain results from every independent stage even when an earlier one fails.
# Aggregate status is still failing: a broken TSan runtime cannot hide ASan/fuzz
# evidence, and a passing fuzz run cannot erase an earlier sanitizer finding.
run_stage() {
  local name="$1"; shift
  local rc
  set +e
  ( set -e; "$@" ) 2>&1 | tee "${evidence_dir}/${name}.log"
  rc=$?
  set -e
  printf '%s\t%s\n' "${name}" "${rc}" >> "${evidence_dir}/status.tsv"
  if (( rc != 0 )); then failed=1; fi
}

: > "${evidence_dir}/status.tsv"
{
  git -C "${repo_dir}" rev-parse HEAD
  "${security_cxx}" --version
  uname -a
} > "${evidence_dir}/provenance.txt"
run_stage asan configure_run asan address,undefined \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON
if [[ "$(uname -s)" != Darwin ]]; then
  run_stage tsan configure_run tsan thread \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON -DENABLE_TSAN=ON
else
  printf 'TSan skipped: unsupported/unreliable for this macOS toolchain; run on Linux.\n'
fi
run_stage fuzz fuzz_run
exit "${failed}"
