#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${SHIELDED_JOBS:-4}"
evidence="${SHIELDED_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-platform}"
mkdir -p "${evidence}"

run_compiler() {
  local name="$1" cc="$2" cxx="$3"
  local build="${repo_dir}/build-shielded-platform-${name}"
  cmake -S "${repo_dir}" -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DDINERO_BUILD_QT=OFF \
    -DCMAKE_C_COMPILER="${cc}" -DCMAKE_CXX_COMPILER="${cxx}"
  cmake --build "${build}" --parallel "${jobs}" --target \
    test_v030_vectors test_shielded_derivation test_shielded_epoch_reset
  ctest --test-dir "${build}" --output-on-failure -R \
    '^(ShieldedV030Vectors|ShieldedDerivation|ShieldedEpochReset)$' \
    | tee "${evidence}/${name}.log"
  # A second build must be a clean incremental no-op.
  cmake --build "${build}" --parallel "${jobs}" \
    --target test_v030_vectors test_shielded_derivation test_shielded_epoch_reset \
    | tee "${evidence}/${name}-incremental.log"
}

run_compiler clang "${CC:-clang}" "${CXX:-clang++}"

gnu_cxx="${SHIELDED_GXX:-g++}"
gnu_cc="${SHIELDED_GCC:-gcc}"
if command -v "${gnu_cxx}" >/dev/null 2>&1 &&
   "${gnu_cxx}" --version | head -1 | grep -qiE 'gcc|g\+\+'; then
  run_compiler gcc "${gnu_cc}" "${gnu_cxx}"
else
  printf 'Genuine GCC unavailable; macOS gcc/g++ aliases do not satisfy this gate.\n' \
    | tee "${evidence}/gcc-unavailable.log"
fi

{
  uname -a
  uname -m
  printf 'requested_arch=%s\n' "${SHIELDED_TARGET_ARCH:-native}"
} >"${evidence}/architecture.txt"

if [[ "${SHIELDED_TARGET_ARCH:-native}" != native &&
      "${SHIELDED_TARGET_ARCH}" != "$(uname -m)" ]]; then
  printf 'Cross-architecture execution requires a hermetic runner for %s.\n' \
    "${SHIELDED_TARGET_ARCH}" | tee "${evidence}/cross-arch-required.log"
  exit 2
fi
