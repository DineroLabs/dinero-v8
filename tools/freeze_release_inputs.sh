#!/usr/bin/env bash
#
# Build fresh Dinero release artifacts from clean HEAD snapshots and verify
# cross-repo commit identity before upload.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DINERO_REPO="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_ROOT="$(cd "${DINERO_REPO}/.." && pwd)"

DINERO_QT_REPO="${DINERO_QT_REPO:-${SRC_ROOT}/dinero-qt}"
DINERO_SOLO_MINER_REPO="${DINERO_SOLO_MINER_REPO:-${SRC_ROOT}/dinero-solo-miner}"
DINERO_STRATUM_REPO="${DINERO_STRATUM_REPO:-${SRC_ROOT}/stratum}"

ARTIFACT_ROOT="${ARTIFACT_ROOT:-$(mktemp -d -t dinero_release_freeze_XXXXXX)}"
WORKTREE_ROOT="${ARTIFACT_ROOT}/sources"
BUILD_ROOT="${ARTIFACT_ROOT}/build"

BUILD_JOBS="${BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
QT_BUILD_TYPE="${QT_BUILD_TYPE:-${BUILD_TYPE}}"
KEEP_ARTIFACTS="${KEEP_ARTIFACTS:-0}"
QT_CMAKE_PREFIX_PATH="${QT_CMAKE_PREFIX_PATH:-}"

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${BLUE}INFO:${NC} $*"; }
warn() { echo -e "${YELLOW}WARN:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
fail() { echo -e "${RED}FAIL:${NC} $*" >&2; exit 1; }

version_equal() {
    python3 - "$1" "$2" <<'PY'
import sys

def parse(value):
    parts = []
    for token in value.strip().split("."):
        try:
            parts.append(int(token))
        except ValueError:
            parts.append(0)
    return parts

left = parse(sys.argv[1])
right = parse(sys.argv[2])
width = max(len(left), len(right))
left.extend([0] * (width - len(left)))
right.extend([0] * (width - len(right)))
print("1" if left == right else "0")
PY
}

repo_macos_target() {
    local repo_path="$1"
    local target_file="${repo_path}/.macos-deployment-target"
    [[ -f "${target_file}" ]] || fail "missing repo macOS target file: ${target_file}"

    local target
    target="$(tr -d '[:space:]' < "${target_file}")"
    [[ -n "${target}" ]] || fail "repo macOS target file is empty: ${target_file}"
    printf '%s\n' "${target}"
}

openssl_repo_dir() {
    local repo_path="$1"
    local base="${repo_path}/third_party/openssl-3.5.7"
    if [[ "$(uname -s)" != "Darwin" ]]; then
        printf '%s\n' "${base}"
        return
    fi

    local host_arch
    host_arch="$(uname -m)"
    if [[ -d "${base}/prebuilt/macos-${host_arch}" ]]; then
        printf '%s\n' "${base}/prebuilt/macos-${host_arch}"
    elif [[ -d "${base}/prebuilt/macos-arm64" ]]; then
        printf '%s\n' "${base}/prebuilt/macos-arm64"
    else
        printf '%s\n' "${base}"
    fi
}

openssl_metadata_target() {
    local openssl_dir="$1"
    local metadata_file="${openssl_dir}/.dinero-build-meta"
    if [[ -f "${metadata_file}" ]]; then
        awk -F= '/^MACOSX_DEPLOYMENT_TARGET=/{print $2; exit}' "${metadata_file}"
    fi
}

openssl_archive_target() {
    local archive_path="$1"
    if [[ "$(uname -s)" == "Darwin" && -f "${archive_path}" ]]; then
        /usr/bin/otool -l "${archive_path}" 2>/dev/null | awk '/minos /{print $2; exit}'
    fi
}

require_repo() {
    local path="$1"
    [[ -d "${path}/.git" ]] || fail "missing git repo: ${path}"
}

git_head() {
    git -C "$1" rev-parse HEAD
}

add_clean_worktree() {
    local source_repo="$1"
    local dest="$2"
    local commit="$3"
    mkdir -p "$(dirname "${dest}")"
    git -C "${source_repo}" worktree add --detach "${dest}" "${commit}" >/dev/null
}

init_submodules_if_needed() {
    local repo_path="$1"
    if [[ -f "${repo_path}/.gitmodules" ]]; then
        info "Initializing submodules in ${repo_path}"
        git -C "${repo_path}" submodule update --init --recursive >/dev/null
    fi
}

prepare_dinero_vendored_openssl() {
    local source_repo="$1"
    local worktree_repo="$2"
    local src_openssl
    src_openssl="$(openssl_repo_dir "${source_repo}")"
    local dst_openssl
    dst_openssl="$(openssl_repo_dir "${worktree_repo}")"
    local source_target_metadata=""
    local source_target_archive=""
    local source_target=""

    if [[ "$(uname -s)" == "Darwin" ]]; then
        source_target_metadata="$(openssl_metadata_target "${src_openssl}")"
        source_target_archive="$(openssl_archive_target "${src_openssl}/libcrypto.a")"
        if [[ -n "${source_target_metadata}" && -n "${source_target_archive}" ]] && [[ "$(version_equal "${source_target_metadata}" "${source_target_archive}")" != "1" ]]; then
            fail "vendored OpenSSL metadata target ${source_target_metadata} does not match archive target ${source_target_archive} in ${source_repo}"
        fi
        if [[ -n "${source_target_archive}" ]]; then
            source_target="${source_target_archive}"
        else
            source_target="${source_target_metadata}"
        fi
    fi

    if [[ -f "${dst_openssl}/libcrypto.a" && -f "${dst_openssl}/libssl.a" ]]; then
        return
    fi

    if [[ -f "${src_openssl}/libcrypto.a" && -f "${src_openssl}/libssl.a" ]] && [[ "$(uname -s)" != "Darwin" || "$(version_equal "${source_target}" "${MACOS_DEPLOYMENT_TARGET}")" == "1" ]]; then
        info "Copying vendored OpenSSL artifacts into frozen dinero worktree"
        rm -rf "${dst_openssl}"
        mkdir -p "$(dirname "${dst_openssl}")"
        cp -a "${src_openssl}" "${dst_openssl}"
        return
    fi

    if [[ -x "${worktree_repo}/scripts/build-openssl-vendored.sh" ]]; then
        info "Building vendored OpenSSL inside frozen dinero worktree"
        (cd "${worktree_repo}" && OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" ./scripts/build-openssl-vendored.sh >/dev/null)
        return
    fi

    fail "vendored OpenSSL is unavailable in ${source_repo} and cannot be built in ${worktree_repo}"
}

resolve_built_binary() {
    local build_dir="$1"
    shift
    local candidate
    for candidate in "$@"; do
        if [[ -x "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    fail "unable to locate built binary in ${build_dir}"
}

remove_worktree() {
    local source_repo="$1"
    local dest="$2"
    git -C "${source_repo}" worktree remove --force "${dest}" >/dev/null 2>&1 || true
}

cleanup() {
    if [[ "${KEEP_ARTIFACTS}" == "1" ]]; then
        warn "Keeping release-freeze artifacts at ${ARTIFACT_ROOT}"
        return
    fi
    remove_worktree "${DINERO_REPO}" "${WORKTREE_ROOT}/dinero"
    remove_worktree "${DINERO_QT_REPO}" "${WORKTREE_ROOT}/dinero-qt"
    remove_worktree "${DINERO_SOLO_MINER_REPO}" "${WORKTREE_ROOT}/dinero-solo-miner"
    remove_worktree "${DINERO_STRATUM_REPO}" "${WORKTREE_ROOT}/stratum"
    rm -rf "${ARTIFACT_ROOT}"
}
trap cleanup EXIT

resolve_qt_prefix_path() {
    if [[ -n "${QT_CMAKE_PREFIX_PATH}" ]]; then
        echo "${QT_CMAKE_PREFIX_PATH}"
        return
    fi

    local cache="${DINERO_QT_REPO}/build/CMakeCache.txt"
    if [[ -f "${cache}" ]]; then
        local value
        value="$(awk -F= '/^CMAKE_PREFIX_PATH:/{print $2; exit}' "${cache}")"
        if [[ -n "${value}" ]]; then
            echo "${value}"
            return
        fi
    fi

    fail "QT_CMAKE_PREFIX_PATH not set and ${cache} does not contain CMAKE_PREFIX_PATH"
}

require_repo "${DINERO_REPO}"
require_repo "${DINERO_QT_REPO}"
require_repo "${DINERO_SOLO_MINER_REPO}"
require_repo "${DINERO_STRATUM_REPO}"

REPO_MACOS_DEPLOYMENT_TARGET="$(repo_macos_target "${DINERO_REPO}")"
if [[ -n "${MACOS_DEPLOYMENT_TARGET:-}" ]]; then
    if [[ "$(version_equal "${MACOS_DEPLOYMENT_TARGET}" "${REPO_MACOS_DEPLOYMENT_TARGET}")" != "1" ]]; then
        fail "MACOS_DEPLOYMENT_TARGET=${MACOS_DEPLOYMENT_TARGET} disagrees with repo policy ${REPO_MACOS_DEPLOYMENT_TARGET}"
    fi
fi
MACOS_DEPLOYMENT_TARGET="${REPO_MACOS_DEPLOYMENT_TARGET}"

DINERO_HEAD="$(git_head "${DINERO_REPO}")"
QT_HEAD="$(git_head "${DINERO_QT_REPO}")"
SOLO_HEAD="$(git_head "${DINERO_SOLO_MINER_REPO}")"
STRATUM_HEAD="$(git_head "${DINERO_STRATUM_REPO}")"

info "Freezing release inputs from clean HEAD snapshots"
info "dinero=${DINERO_HEAD}"
info "dinero-qt=${QT_HEAD}"
info "dinero-solo-miner=${SOLO_HEAD}"
info "stratum=${STRATUM_HEAD}"
info "artifacts=${ARTIFACT_ROOT}"

add_clean_worktree "${DINERO_REPO}" "${WORKTREE_ROOT}/dinero" "${DINERO_HEAD}"
add_clean_worktree "${DINERO_QT_REPO}" "${WORKTREE_ROOT}/dinero-qt" "${QT_HEAD}"
add_clean_worktree "${DINERO_SOLO_MINER_REPO}" "${WORKTREE_ROOT}/dinero-solo-miner" "${SOLO_HEAD}"
add_clean_worktree "${DINERO_STRATUM_REPO}" "${WORKTREE_ROOT}/stratum" "${STRATUM_HEAD}"

init_submodules_if_needed "${WORKTREE_ROOT}/dinero"
init_submodules_if_needed "${WORKTREE_ROOT}/dinero-qt"
init_submodules_if_needed "${WORKTREE_ROOT}/dinero-solo-miner"
init_submodules_if_needed "${WORKTREE_ROOT}/stratum"
prepare_dinero_vendored_openssl "${DINERO_REPO}" "${WORKTREE_ROOT}/dinero"

mkdir -p "${BUILD_ROOT}/dinero" "${BUILD_ROOT}/dinero-qt" "${BUILD_ROOT}/dinero-solo-miner" "${BUILD_ROOT}/stratum"
QT_CMAKE_PREFIX_PATH="$(resolve_qt_prefix_path)"

info "Building dinerod + CLI/miners"
if [[ "$(uname -s)" == "Darwin" ]]; then
    cmake -S "${WORKTREE_ROOT}/dinero" -B "${BUILD_ROOT}/dinero" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}" >/dev/null
else
    cmake -S "${WORKTREE_ROOT}/dinero" -B "${BUILD_ROOT}/dinero" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
fi
cmake --build "${BUILD_ROOT}/dinero" --target dinerod dinero-cli dinero-miner dinero-stratum-worker dinero-gpu-miner -j"${BUILD_JOBS}"

info "Building standalone solo miner"
cmake -S "${WORKTREE_ROOT}/dinero-solo-miner" -B "${BUILD_ROOT}/dinero-solo-miner" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
cmake --build "${BUILD_ROOT}/dinero-solo-miner" --target dinero-solo-miner-cli -j"${BUILD_JOBS}"
SOLO_MINER_BIN="$(resolve_built_binary "${BUILD_ROOT}/dinero-solo-miner" \
    "${BUILD_ROOT}/dinero-solo-miner/dinero-solo-miner-cli" \
    "${BUILD_ROOT}/dinero-solo-miner/dinero-solo-miner" \
    "${BUILD_ROOT}/dinero-solo-miner/bin/dinero-solo-miner-cli" \
    "${BUILD_ROOT}/dinero-solo-miner/bin/dinero-solo-miner")"

info "Building standalone stratum"
cmake -S "${WORKTREE_ROOT}/stratum" -B "${BUILD_ROOT}/stratum" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" >/dev/null
cmake --build "${BUILD_ROOT}/stratum" --target dinero-stratum -j"${BUILD_JOBS}"
STRATUM_BIN="$(resolve_built_binary "${BUILD_ROOT}/stratum" \
    "${BUILD_ROOT}/stratum/dinero-stratum" \
    "${BUILD_ROOT}/stratum/bin/dinero-stratum")"

info "Configuring dinero-qt bundle"
cmake -S "${WORKTREE_ROOT}/dinero-qt" -B "${BUILD_ROOT}/dinero-qt" \
    -DCMAKE_BUILD_TYPE="${QT_BUILD_TYPE}" \
    -DCMAKE_PREFIX_PATH="${QT_CMAKE_PREFIX_PATH}" \
    -DDINERO_SOURCE_ROOT="${WORKTREE_ROOT}/dinero" \
    -DDINERO_SOLO_MINER_SOURCE_ROOT="${WORKTREE_ROOT}/dinero-solo-miner" \
    -DDINERO_STRATUM_SOURCE_ROOT="${WORKTREE_ROOT}/stratum" \
    -DDINEROD_BINARY="${BUILD_ROOT}/dinero/dinerod" \
    -DGPU_MINER_BINARY="${BUILD_ROOT}/dinero/dinero-gpu-miner" >/dev/null

info "Building dinero-qt"
cmake --build "${BUILD_ROOT}/dinero-qt" --target dinero-qt -j"${BUILD_JOBS}"

APP_BUNDLE="${BUILD_ROOT}/dinero-qt/bin/dinero-qt.app"
[[ -d "${APP_BUNDLE}" ]] || fail "missing Qt app bundle: ${APP_BUNDLE}"

info "Verifying bundled release identities"
python3 "${WORKTREE_ROOT}/dinero-qt/tools/verify_release_identities.py" \
    --bundle "${APP_BUNDLE}" \
    --dinero-repo "${WORKTREE_ROOT}/dinero" \
    --qt-repo "${WORKTREE_ROOT}/dinero-qt" \
    --stratum-bin "${STRATUM_BIN}" \
    --stratum-repo "${WORKTREE_ROOT}/stratum" \
    | tee "${ARTIFACT_ROOT}/release_identity_check.txt"

info "Writing freeze manifest"
python3 - "${ARTIFACT_ROOT}" \
    "${DINERO_HEAD}" "${QT_HEAD}" "${SOLO_HEAD}" "${STRATUM_HEAD}" \
    "${BUILD_ROOT}/dinero/dinerod" \
    "${BUILD_ROOT}/dinero/dinero-cli" \
    "${BUILD_ROOT}/dinero/dinero-miner" \
    "${BUILD_ROOT}/dinero/dinero-stratum-worker" \
    "${BUILD_ROOT}/dinero/dinero-gpu-miner" \
    "${STRATUM_BIN}" \
    "${SOLO_MINER_BIN}" \
    "${APP_BUNDLE}" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

artifact_root = pathlib.Path(sys.argv[1])
dinero_head, qt_head, solo_head, stratum_head = sys.argv[2:6]
binary_paths = {
    "dinerod": pathlib.Path(sys.argv[6]),
    "dinero-cli": pathlib.Path(sys.argv[7]),
    "dinero-miner": pathlib.Path(sys.argv[8]),
    "dinero-stratum-worker": pathlib.Path(sys.argv[9]),
    "dinero-gpu-miner": pathlib.Path(sys.argv[10]),
    "dinero-stratum": pathlib.Path(sys.argv[11]),
    "dinero-solo-miner-cli": pathlib.Path(sys.argv[12]),
}
bundle_path = pathlib.Path(sys.argv[13])

def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def version_output(path: pathlib.Path) -> str:
    attempts = [
        [str(path), "--version"],
        [str(path), "-version"],
        [str(path), "help"],
        [str(path), "--help"],
    ]
    fallback = ""
    for argv in attempts:
        proc = subprocess.run(argv, capture_output=True, text=True)
        output = (proc.stdout or "") + (proc.stderr or "")
        output = output.strip()
        if output and not fallback:
            fallback = output
        if proc.returncode == 0 and output:
            return output
    return fallback or "version-unavailable"

manifest = {
    "repo_heads": {
        "dinero": dinero_head,
        "dinero-qt": qt_head,
        "dinero-solo-miner": solo_head,
        "stratum": stratum_head,
    },
    "binaries": {},
    "qt_bundle": {
        "path": str(bundle_path),
    },
}

for name, path in binary_paths.items():
    manifest["binaries"][name] = {
        "path": str(path),
        "sha256": sha256(path),
        "version": version_output(path),
    }

(artifact_root / "release_freeze_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY

pass "Release inputs frozen and verified"
echo "artifacts=${ARTIFACT_ROOT}"
