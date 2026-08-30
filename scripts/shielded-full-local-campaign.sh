#!/usr/bin/env bash
set -uo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
evidence_dir="${SHIELDED_FULL_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-full-campaign/${stamp}}"
build_dir="${SHIELDED_FULL_BUILD_DIR:-${repo_dir}/build-shielded-readiness}"
jobs="${SHIELDED_JOBS:-4}"
mkdir -p "${evidence_dir}/logs"

failures=0
run_phase() {
  local name="$1"; shift
  printf '==> %s\n' "${name}"
  if "$@" >"${evidence_dir}/logs/${name}.log" 2>&1; then
    printf 'PASS %s\n' "${name}" | tee "${evidence_dir}/${name}.status"
  else
    local rc=$?
    printf 'FAIL %s rc=%s\n' "${name}" "${rc}" | tee "${evidence_dir}/${name}.status"
    failures=$((failures + 1))
  fi
}

run_live_core() {
  local dinerod="${SHIELDED_DINEROD:-${repo_dir}/build-qt-release-ui/dinerod}"
  local helpers="${repo_dir}/build-qt-release-ui/tests/integration"
  DINEROD="${dinerod}" "${repo_dir}/tests/integration/test_shielded_rpc_transfer_addressed_detect_e2e.sh" &&
  DINEROD="${dinerod}" "${repo_dir}/tests/integration/test_shielded_rpc_transfer_multi_e2e.sh" &&
  DINEROD="${dinerod}" "${repo_dir}/tests/integration/test_shielded_wallet_reject_rollback.sh" &&
  DINEROD="${dinerod}" SHIELDED_TX_BUILDER="${helpers}/shielded_tx_builder" \
    SHIELDED_PROBE="${helpers}/shielded_tip_marker_probe" \
    "${repo_dir}/tests/integration/test_shielded_daemon_restart_equivalence.sh" &&
  DINEROD="${dinerod}" SHIELDED_TX_BUILDER="${helpers}/shielded_tx_builder" \
    SHIELDED_PROBE="${helpers}/shielded_tip_marker_probe" \
    "${repo_dir}/tests/integration/test_shielded_reorg_disconnect_restart_equivalence.sh"
}

run_mobile() {
  SHIELDED_NATIVE_MOBILE_EVIDENCE_DIR="${evidence_dir}/native-mobile" \
    "${repo_dir}/scripts/shielded-native-mobile-readiness.sh"
  if [[ -n "${ANDROID_SERIAL:-}" ]]; then
    ANDROID_SERIAL="${ANDROID_SERIAL}" \
      /Users/haydarevich/src/apps/DineroDPIAndroid/scripts/test-device-matrix.sh
  fi
  if [[ -n "${IOS_DEVICE_ID:-}" ]]; then
    local ios_repo="${DINERODPI_IOS_REPO:-/Users/haydarevich/src/apps/DineroDPI}"
    local derived="${SHIELDED_IOS_DERIVED_DATA:-${repo_dir}/build-shielded-ios-device-campaign}"
    xcodebuild -project "${ios_repo}/DineroDPI/DineroDPI.xcodeproj" -scheme DineroDPI \
      -configuration Debug -destination "platform=iOS,id=${IOS_DEVICE_ID}" \
      -derivedDataPath "${derived}" build
    xcrun devicectl device install app --device "${IOS_DEVICE_ID}" \
      "${derived}/Build/Products/Debug-iphoneos/DineroDPI.app"
    xcrun devicectl device process launch --device "${IOS_DEVICE_ID}" Mr.No.DineroDPI
  fi
}

run_phase readiness env SHIELDED_EVIDENCE_DIR="${evidence_dir}/readiness" \
  SHIELDED_JOBS="${jobs}" "${repo_dir}/scripts/shielded-readiness.sh" "${build_dir}"
run_phase live-core run_live_core
run_phase determinism env SHIELDED_EVIDENCE_DIR="${evidence_dir}/determinism" \
  SHIELDED_JOBS="${jobs}" "${repo_dir}/scripts/shielded-determinism-matrix.sh"
run_phase platform env SHIELDED_EVIDENCE_DIR="${evidence_dir}/platform" \
  SHIELDED_JOBS="${jobs}" "${repo_dir}/scripts/shielded-platform-matrix.sh"
run_phase performance env SHIELDED_PERF_EVIDENCE_DIR="${evidence_dir}/performance" \
  SHIELDED_JOBS="${jobs}" "${repo_dir}/scripts/shielded-performance-campaign.sh"
run_phase security env SHIELDED_FUZZ_SECONDS="${SHIELDED_FUZZ_SECONDS:-300}" \
  SHIELDED_JOBS="${jobs}" "${repo_dir}/scripts/shielded-security-matrix.sh"
run_phase native-mobile run_mobile

{
  printf 'timestamp=%s\n' "${stamp}"
  printf 'commit=%s\n' "$(git -C "${repo_dir}" rev-parse HEAD)"
  printf 'describe=%s\n' "$(git -C "${repo_dir}" describe --tags --always --dirty)"
  printf 'architecture=%s\n' "$(uname -m)"
  printf 'failures=%s\n' "${failures}"
} >"${evidence_dir}/summary.txt"
find "${evidence_dir}" -type f ! -name SHA256SUMS -print | LC_ALL=C sort | \
  while IFS= read -r evidence_file; do shasum -a 256 "${evidence_file}"; done \
  >"${evidence_dir}/SHA256SUMS"

printf 'Full shielded campaign evidence: %s\n' "${evidence_dir}"
exit "${failures}"
