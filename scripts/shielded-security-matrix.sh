#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${SHIELDED_JOBS:-4}"
duration="${SHIELDED_FUZZ_SECONDS:-60}"

configure_run() {
  local name="$1"; shift
  local dir="${repo_dir}/build-shielded-${name}"
  cmake -S "${repo_dir}" -B "${dir}" -G Ninja -DBUILD_TESTING=ON \
    -DDINERO_BUILD_QT=OFF "$@"
  cmake --build "${dir}" --parallel "${jobs}" --target test_anchor_history \
    test_shielded_serialization test_shielded_derivation
  ctest --test-dir "${dir}" --output-on-failure -R \
    '^(AnchorHistory|ShieldedSerialization|ShieldedDerivation)$'
}

configure_run asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON

if [[ "$(uname -s)" != Darwin ]]; then
  configure_run tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON -DENABLE_TSAN=ON
else
  printf 'TSan skipped: unsupported/unreliable for this macOS toolchain; run on Linux.\n'
fi

fuzz_dir="${repo_dir}/build-shielded-fuzz-llvm"
fuzz_cc="${SHIELDED_FUZZ_CC:-clang}"
fuzz_cxx="${SHIELDED_FUZZ_CXX:-clang++}"
fuzz_sanitizers="fuzzer,address,undefined"
if [[ "$(uname -s)" == Darwin ]] && command -v brew >/dev/null 2>&1; then
  llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
  if [[ -x "${llvm_prefix}/bin/clang++" ]]; then
    fuzz_cc="${llvm_prefix}/bin/clang"
    fuzz_cxx="${llvm_prefix}/bin/clang++"
    fuzz_sanitizers="fuzzer,undefined"
  fi
fi
cmake -S "${repo_dir}" -B "${fuzz_dir}" -G Ninja -DBUILD_TESTING=ON \
  -DDINERO_BUILD_QT=OFF -DENABLE_FUZZING=ON \
  -DDINERO_FUZZ_SANITIZERS="${fuzz_sanitizers}" \
  -DCMAKE_C_COMPILER="${fuzz_cc}" -DCMAKE_CXX_COMPILER="${fuzz_cxx}"
cmake --build "${fuzz_dir}" --parallel "${jobs}" --target fuzz_shielded_surfaces
"${fuzz_dir}/fuzz/fuzz_shielded_surfaces" "${fuzz_dir}/fuzz_corpus/shielded" \
  -artifact_prefix="${fuzz_dir}/fuzz_crashes/shielded/" \
  -max_len=200000 -timeout=5 -max_total_time="${duration}" -print_final_stats=1
