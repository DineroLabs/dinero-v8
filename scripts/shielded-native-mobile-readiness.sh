#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
evidence_dir="${SHIELDED_NATIVE_MOBILE_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-native-mobile}"
ios_root="${DINERODPI_IOS_ROOT:-/Users/haydarevich/src/apps/DineroDPI/DineroDPI}"
android_root="${DINERODPI_ANDROID_ROOT:-/Users/haydarevich/src/apps/DineroDPIAndroid}"
mkdir -p "${evidence_dir}"

ios_project="${ios_root}/DineroDPI.xcodeproj"
if [[ ! -d "${ios_project}" ]]; then
  printf 'FAIL: native Xcode DineroDPI project not found: %s\n' "${ios_project}" >&2
  exit 2
fi
if [[ ! -x "${android_root}/gradlew" ]]; then
  printf 'FAIL: native Android DineroDPI Gradle wrapper not found: %s\n' "${android_root}/gradlew" >&2
  exit 2
fi

xcodebuild -project "${ios_project}" -scheme DineroDPI -configuration Debug \
  -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build \
  | tee "${evidence_dir}/xcode-simulator-build.log"

(cd "${android_root}" && ./gradlew testDebugUnitTest assembleDebug) \
  | tee "${evidence_dir}/android-debug-build.log"

{
  xcodebuild -version
  "${android_root}/gradlew" --version
  printf 'iOS project: %s\nAndroid project: %s\n' "${ios_project}" "${android_root}"
} > "${evidence_dir}/provenance.txt"

printf 'Native mobile evidence: %s\n' "${evidence_dir}"
