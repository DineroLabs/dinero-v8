#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/depends/tor-expert-bundle/manifest.env"

arch="${1:-$(uname -m)}"
output="${2:-${ROOT_DIR}/build/tor-expert-bundle}"
case "${arch}" in
  arm64|aarch64)
    archive_arch="aarch64"
    expected="${TOR_MACOS_AARCH64_SHA256}"
    ;;
  x86_64)
    archive_arch="x86_64"
    expected="${TOR_MACOS_X86_64_SHA256}"
    ;;
  *) echo "unsupported Tor Expert Bundle architecture: ${arch}" >&2; exit 2 ;;
esac

command -v gpg >/dev/null || {
  echo "gpg is required to verify the Tor upstream signature" >&2
  exit 2
}

stage="$(mktemp -d)"
trap 'rm -rf "${stage}"' EXIT
base="https://archive.torproject.org/tor-package-archive/torbrowser/${TOR_BUNDLE_VERSION}"
name="tor-expert-bundle-macos-${archive_arch}-${TOR_BUNDLE_VERSION}.tar.gz"
curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
  --output "${stage}/${name}" "${base}/${name}"
curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
  --output "${stage}/${name}.asc" "${base}/${name}.asc"

actual="$(shasum -a 256 "${stage}/${name}" | awk '{print $1}')"
[[ "${actual}" == "${expected}" ]] || {
  echo "Tor bundle checksum mismatch" >&2
  exit 1
}

GNUPGHOME="${stage}/gnupg"
export GNUPGHOME
mkdir -m 700 "${GNUPGHOME}"
gpg --batch --auto-key-locate nodefault,wkd --locate-keys torbrowser@torproject.org >/dev/null 2>&1
fingerprint="$(gpg --batch --with-colons --fingerprint torbrowser@torproject.org |
  awk -F: '$1 == "fpr" {print $10; exit}')"
[[ "${fingerprint}" == "${TOR_SIGNING_FINGERPRINT}" ]] || {
  echo "unexpected Tor signing-key fingerprint" >&2
  exit 1
}
gpg --batch --verify "${stage}/${name}.asc" "${stage}/${name}"

mkdir -p "${output}"
tar -xzf "${stage}/${name}" -C "${output}"
printf '%s\n' "${TOR_BUNDLE_VERSION} (tor ${TOR_VERSION})" > "${output}/DINERO_TOR_VERSION"
if [[ "$(uname -s)" == "Darwin" ]]; then
  command -v codesign >/dev/null || {
    echo "codesign is required for the macOS Tor bundle" >&2
    exit 2
  }
  # The upstream archive is unsigned. macOS terminates the extracted Mach-O
  # before main(); give nested code an ad-hoc development signature here.
  # Release packaging replaces this when it signs the app inside-out.
  shopt -s nullglob
  mac_code=("${output}/tor/tor" "${output}/tor/"*.dylib
            "${output}/tor/pluggable_transports/lyrebird"
            "${output}/tor/pluggable_transports/conjure-client")
  for binary in "${mac_code[@]}"; do
    [[ -f "${binary}" ]] && codesign --force --sign - "${binary}" >/dev/null
  done
  shopt -u nullglob
  codesign --verify --strict "${output}/tor/tor"
fi
"${output}/tor/tor" --version | grep -F "Tor version ${TOR_VERSION}" >/dev/null
echo "Verified Tor Expert Bundle installed at ${output}"
