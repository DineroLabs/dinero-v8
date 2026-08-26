#!/usr/bin/env bash
# Build the complete desktop stack from immutable source snapshots and prove
# that every miner embedded in dinero-qt came from those exact snapshots.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DINERO_REPO="${DINERO_REPO:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
SRC_ROOT="$(cd "${DINERO_REPO}/.." && pwd)"
DINERO_SV2_REPO="${DINERO_SV2_REPO:-${SRC_ROOT}/dinero-sv2}"
SV2_REF_FILE="${DINERO_REPO}/.github/workflows/desktop-release.yml"
EXPECTED_SV2_HEAD="${EXPECTED_SV2_HEAD:-$(awk '/^[[:space:]]*SV2_REF:/{print $2; exit}' "${SV2_REF_FILE}")}"
ARTIFACT_ROOT="${ARTIFACT_ROOT:-$(mktemp -d -t dinero_release_freeze_XXXXXX)}"
WORKTREE_ROOT="${ARTIFACT_ROOT}/sources"
BUILD_ROOT="${ARTIFACT_ROOT}/build"
BUILD_JOBS="${BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
KEEP_ARTIFACTS="${KEEP_ARTIFACTS:-0}"
QT_CMAKE_PREFIX_PATH="${QT_CMAKE_PREFIX_PATH:-}"
RELEASE_TAG="${RELEASE_TAG:-$(git -C "${DINERO_REPO}" describe --tags --exact-match HEAD 2>/dev/null || git -C "${DINERO_REPO}" describe --always --dirty)}"
RELEASE_VERSION="${RELEASE_TAG#v}"

info() { printf 'INFO: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

require_repo() {
  [[ -d "$1/.git" || -f "$1/.git" ]] || fail "missing git repository: $1"
}

remove_worktree() {
  git -C "$1" worktree remove --force "$2" >/dev/null 2>&1 || true
}

cleanup() {
  if [[ "${KEEP_ARTIFACTS}" == "1" ]]; then
    info "keeping release-freeze artifacts at ${ARTIFACT_ROOT}"
    return
  fi
  remove_worktree "${DINERO_REPO}" "${WORKTREE_ROOT}/dinero"
  remove_worktree "${DINERO_SV2_REPO}" "${WORKTREE_ROOT}/dinero-sv2"
  rm -rf "${ARTIFACT_ROOT}"
}
trap cleanup EXIT

resolve_qt_prefix() {
  if [[ -n "${QT_CMAKE_PREFIX_PATH}" ]]; then
    printf '%s\n' "${QT_CMAKE_PREFIX_PATH}"
    return
  fi
  local cache candidate
  for cache in \
    "${DINERO_REPO}/build-qt-release-ui/CMakeCache.txt" \
    "${DINERO_REPO}/build/CMakeCache.txt"; do
    [[ -f "${cache}" ]] || continue
    candidate="$(awk -F= '/^CMAKE_PREFIX_PATH:/{print $2; exit}' "${cache}")"
    if [[ -n "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return
    fi
    candidate="$(awk -F= '/^Qt6_DIR:PATH=/{print $2; exit}' "${cache}")"
    if [[ -n "${candidate}" ]]; then
      (cd "${candidate}/../../.." && pwd)
      return
    fi
  done
  if command -v brew >/dev/null 2>&1 && brew --prefix qt >/dev/null 2>&1; then
    brew --prefix qt
    return
  fi
  fail "QT_CMAKE_PREFIX_PATH is unset and no Qt prefix could be discovered"
}

sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

# Signing rewrites Mach-O load commands and the __LINKEDIT signature blob. To
# prove that a bundled executable is the one we built, hash every loadable
# Mach-O section instead of comparing the mutable signature envelope. For
# non-Mach-O platforms this falls back to the ordinary whole-file digest.
content_sha256() {
  python3 - "$1" <<'PY'
import hashlib
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
if data[:4] != b"\xcf\xfa\xed\xfe":
    print(hashlib.sha256(data).hexdigest())
    raise SystemExit

ncmds = struct.unpack_from("<I", data, 16)[0]
cursor = 32
h = hashlib.sha256()
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from("<II", data, cursor)
    if cmdsize < 8 or cursor + cmdsize > len(data):
        raise SystemExit("malformed Mach-O load command")
    if cmd == 0x19:  # LC_SEGMENT_64
        nsects = struct.unpack_from("<I", data, cursor + 64)[0]
        section = cursor + 72
        for _ in range(nsects):
            sectname = data[section:section + 16].rstrip(b"\0")
            segname = data[section + 16:section + 32].rstrip(b"\0")
            size = struct.unpack_from("<Q", data, section + 40)[0]
            offset = struct.unpack_from("<I", data, section + 48)[0]
            section_type = struct.unpack_from("<I", data, section + 64)[0] & 0xff
            if section_type not in (0x1, 0x0c, 0x12):
                if offset + size > len(data):
                    raise SystemExit("Mach-O section exceeds file")
                h.update(segname + b"\0" + sectname + b"\0")
                h.update(struct.pack("<Q", size))
                h.update(data[offset:offset + size])
            section += 80
    cursor += cmdsize
print(h.hexdigest())
PY
}

require_repo "${DINERO_REPO}"
require_repo "${DINERO_SV2_REPO}"
[[ "${EXPECTED_SV2_HEAD}" =~ ^[0-9a-f]{40}$ ]] || fail "invalid pinned SV2 commit in ${SV2_REF_FILE}"
git -C "${DINERO_SV2_REPO}" cat-file -e "${EXPECTED_SV2_HEAD}^{commit}" 2>/dev/null || \
  fail "pinned SV2 commit is unavailable locally: ${EXPECTED_SV2_HEAD}"

DINERO_HEAD="$(git -C "${DINERO_REPO}" rev-parse HEAD)"
mkdir -p "${WORKTREE_ROOT}" "${BUILD_ROOT}"
git -C "${DINERO_REPO}" worktree add --detach "${WORKTREE_ROOT}/dinero" "${DINERO_HEAD}" >/dev/null
git -C "${DINERO_SV2_REPO}" worktree add --detach "${WORKTREE_ROOT}/dinero-sv2" "${EXPECTED_SV2_HEAD}" >/dev/null
git -C "${WORKTREE_ROOT}/dinero" submodule update --init --recursive >/dev/null

[[ -z "$(git -C "${WORKTREE_ROOT}/dinero" status --porcelain)" ]] || fail "frozen Dinero source is dirty"
[[ -z "$(git -C "${WORKTREE_ROOT}/dinero-sv2" status --porcelain)" ]] || fail "frozen SV2 source is dirty"
[[ "$(git -C "${WORKTREE_ROOT}/dinero-sv2" rev-parse HEAD)" == "${EXPECTED_SV2_HEAD}" ]] || fail "SV2 checkout identity mismatch"

info "dinero=${DINERO_HEAD}"
info "dinero-sv2=${EXPECTED_SV2_HEAD}"
info "release_tag=${RELEASE_TAG}"
info "artifacts=${ARTIFACT_ROOT}"

info "building pinned SV2 miners"
cargo build --manifest-path "${WORKTREE_ROOT}/dinero-sv2/Cargo.toml" --release --locked
SV2_BUILD="${WORKTREE_ROOT}/dinero-sv2/target/release"
for name in dinero-sv2-miner dinero-sv2-gpu-miner; do
  [[ -x "${SV2_BUILD}/${name}" ]] || fail "missing pinned SV2 binary: ${name}"
done

info "building vendored OpenSSL"
(cd "${WORKTREE_ROOT}/dinero" && OPENSSL_REBUILD=1 bash scripts/build-openssl-vendored.sh >/dev/null)

QT_CMAKE_PREFIX_PATH="$(resolve_qt_prefix)"
cmake_args=(
  -S "${WORKTREE_ROOT}/dinero"
  -B "${BUILD_ROOT}/dinero"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DCMAKE_PREFIX_PATH="${QT_CMAKE_PREFIX_PATH}"
  -DDINERO_USE_VENDORED_DEPS=ON
  -DDINERO_BUILD_QT=ON
  -DDINERO_BUILD_MINER=ON
  -DDINERO_BUILD_SEEDER=ON
  -DENABLE_GPU_MINING=ON
  -DENABLE_GRPC=OFF
  -DDINERO_ENABLE_PORTMAPPING=OFF
  -DDINERO_ENABLE_QUIC=ON
  -DDINERO_RELEASE_TAG="${RELEASE_VERSION}"
  -DDINERO_SV2_SOURCE_ROOT="${WORKTREE_ROOT}/dinero-sv2"
)
if [[ "$(uname -s)" == "Darwin" ]]; then
  cmake_args+=(
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$(tr -d '[:space:]' < "${WORKTREE_ROOT}/dinero/.macos-deployment-target")"
    -DMINER_ENABLE_METAL=ON
    -DMINER_ENABLE_OPENCL=ON
  )
fi

info "configuring and building the complete desktop stack"
cmake "${cmake_args[@]}" >/dev/null
cmake --build "${BUILD_ROOT}/dinero" -j"${BUILD_JOBS}" --target \
  dinerod dinero-cli dinero-qt dinero-miner dinero-stratum-worker \
  dinero-wallet-cli dinero-gpu-miner dinero-solo-miner-cli dinero-seeder

APP="${BUILD_ROOT}/dinero/bin/dinero-qt.app"
[[ -d "${APP}" ]] || fail "missing Qt app bundle: ${APP}"

manifest_rows="${ARTIFACT_ROOT}/binary_rows.tsv"
: > "${manifest_rows}"
while IFS='|' read -r name source_path; do
  [[ -n "${name}" ]] || continue
  [[ -x "${source_path}" ]] || fail "missing release binary: ${source_path}"
  source_hash="$(sha256 "${source_path}")"
  source_content_hash="$(content_sha256 "${source_path}")"
  bundle_hashes=""
  for bundle_dir in MacOS Resources; do
    bundled="${APP}/Contents/${bundle_dir}/${name}"
    [[ -x "${bundled}" ]] || fail "bundle omitted ${bundle_dir}/${name}"
    bundled_hash="$(sha256 "${bundled}")"
    bundled_content_hash="$(content_sha256 "${bundled}")"
    [[ "${source_content_hash}" == "${bundled_content_hash}" ]] || fail "bundle code identity mismatch for ${bundle_dir}/${name}"
    bundle_hashes="${bundle_hashes}${bundle_hashes:+,}${bundle_dir}=${bundled_hash}"
  done
  printf '%s\t%s\t%s\t%s\t%s\n' \
    "${name}" "${source_hash}" "${source_content_hash}" "${bundle_hashes}" "${source_path}" >> "${manifest_rows}"
done <<EOF
dinerod|${BUILD_ROOT}/dinero/dinerod
dinero-cli|${BUILD_ROOT}/dinero/dinero-cli
dinero-miner|${BUILD_ROOT}/dinero/dinero-miner
dinero-stratum-worker|${BUILD_ROOT}/dinero/dinero-stratum-worker
dinero-gpu-miner|${BUILD_ROOT}/dinero/dinero-gpu-miner
dinero-solo-miner|${BUILD_ROOT}/dinero/miner/dinero-solo-miner
dinero-seeder|${BUILD_ROOT}/dinero/seeder/dinero-seeder
dinero-sv2-miner|${SV2_BUILD}/dinero-sv2-miner
dinero-sv2-gpu-miner|${SV2_BUILD}/dinero-sv2-gpu-miner
EOF

python3 - "${ARTIFACT_ROOT}" "${DINERO_HEAD}" "${EXPECTED_SV2_HEAD}" "${RELEASE_TAG}" "${APP}" "${manifest_rows}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
rows = pathlib.Path(sys.argv[6]).read_text().splitlines()
binaries = {}
for row in rows:
    name, digest, content_digest, bundle_hashes, path = row.split("\t", 4)
    binaries[name] = {
        "source_sha256": digest,
        "code_content_sha256": content_digest,
        "bundle_sha256": dict(item.split("=", 1) for item in bundle_hashes.split(",")),
        "source_path": path,
    }
manifest = {
    "release_tag": sys.argv[4],
    "repo_heads": {"dinero-v8": sys.argv[2], "dinero-sv2": sys.argv[3]},
    "qt_bundle": sys.argv[5],
    "binaries": binaries,
}
(root / "release_freeze_manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
PY

pass "release inputs frozen; all embedded binary identities match"
printf 'artifacts=%s\n' "${ARTIFACT_ROOT}"
