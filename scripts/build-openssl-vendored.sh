#!/usr/bin/env bash
# ============================================================================
# Build vendored OpenSSL for DineroCoin
# Produces static libraries for portable binaries
# Platform: macOS, Linux (x86_64, ARM64)
# ============================================================================

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.6}"
OPENSSL_DIR="${OPENSSL_SOURCE_DIR:-$PROJECT_ROOT/third_party/openssl-${OPENSSL_VERSION}}"
MACOS_TARGET_FILE="$PROJECT_ROOT/.macos-deployment-target"

known_openssl_source_sha256() {
    case "$1" in
        3.5.6)
            printf '%s\n' "deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736"
            ;;
        *)
            return 1
            ;;
    esac
}

echo -e "${BLUE}--------------------------------------------------------${NC}"
echo -e "${BLUE}Building vendored OpenSSL ${OPENSSL_VERSION} for DineroCoin${NC}"
echo -e "${BLUE}--------------------------------------------------------${NC}"

# Download the pinned source tarball when the selected source directory is not
# already present. OPENSSL_SOURCE_DIR remains strict: if callers pass it, they
# are responsible for making it exist.
ensure_openssl_source() {
    if [[ -f "$OPENSSL_DIR/Configure" ]]; then
        return
    fi

    if [[ -n "${OPENSSL_SOURCE_DIR:-}" ]]; then
        echo -e "${RED}Error: OPENSSL_SOURCE_DIR was provided but does not contain OpenSSL source: $OPENSSL_DIR${NC}"
        exit 1
    fi

    local expected_sha
    if ! expected_sha="$(known_openssl_source_sha256 "$OPENSSL_VERSION")"; then
        echo -e "${RED}Error: OpenSSL source missing at $OPENSSL_DIR and no pinned SHA256 is known for OPENSSL_VERSION=$OPENSSL_VERSION${NC}"
        exit 1
    fi

    local tarball="$PROJECT_ROOT/third_party/openssl-${OPENSSL_VERSION}.tar.gz"
    local url="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    mkdir -p "$PROJECT_ROOT/third_party"
    if [[ ! -f "$tarball" ]]; then
        echo -e "${BLUE}Downloading OpenSSL ${OPENSSL_VERSION} source...${NC}"
        curl -L "$url" -o "$tarball"
    fi

    local actual_sha
    actual_sha="$(python3 - "$tarball" <<'PY'
import hashlib
import sys

h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b""):
        h.update(chunk)
print(h.hexdigest())
PY
)"
    if [[ "$actual_sha" != "$expected_sha" ]]; then
        echo -e "${RED}Error: SHA256 mismatch for $tarball${NC}"
        echo "Expected: $expected_sha"
        echo "Actual:   $actual_sha"
        exit 1
    fi

    echo -e "${BLUE}Extracting OpenSSL ${OPENSSL_VERSION} source...${NC}"
    tar -xzf "$tarball" -C "$PROJECT_ROOT/third_party"
    if [[ ! -d "$OPENSSL_DIR" ]]; then
        echo -e "${RED}Error: OpenSSL extraction completed but expected directory is missing: $OPENSSL_DIR${NC}"
        exit 1
    fi
}

ensure_openssl_source

# Check if OpenSSL source exists
if [ ! -d "$OPENSSL_DIR" ]; then
    echo -e "${RED}Error: OpenSSL source directory not found at $OPENSSL_DIR${NC}"
    exit 1
fi

# Detect platform
OS="$(uname -s)"
ARCH="$(uname -m)"
OPENSSL_REBUILD="${OPENSSL_REBUILD:-0}"

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

read_repo_macos_target() {
    if [[ ! -f "$MACOS_TARGET_FILE" ]]; then
        echo -e "${RED}Error: missing repo macOS target file at $MACOS_TARGET_FILE${NC}" >&2
        exit 1
    fi

    local target
    target="$(tr -d '[:space:]' < "$MACOS_TARGET_FILE")"
    if [[ -z "$target" ]]; then
        echo -e "${RED}Error: repo macOS target file is empty: $MACOS_TARGET_FILE${NC}" >&2
        exit 1
    fi
    printf '%s\n' "$target"
}

read_metadata_target() {
    if [[ -f "$BUILD_METADATA_FILE" ]]; then
        awk -F= '/^MACOSX_DEPLOYMENT_TARGET=/{print $2; exit}' "$BUILD_METADATA_FILE"
    fi
}

read_metadata_field() {
    local key="$1"
    if [[ -f "$BUILD_METADATA_FILE" ]]; then
        awk -F= -v k="$key" '$1 == k {print $2; exit}' "$BUILD_METADATA_FILE"
    fi
}

detect_archive_target() {
    local archive="$1"
    if [[ "$OS" == "Darwin" && -f "$archive" ]]; then
        /usr/bin/otool -l "$archive" 2>/dev/null | awk '/minos /{print $2; exit}'
    fi
}

default_output_dir() {
    if [[ -n "${OPENSSL_OUTPUT_DIR:-}" ]]; then
        printf '%s\n' "${OPENSSL_OUTPUT_DIR}"
        return
    fi

    if [[ "$OS" == "Darwin" ]]; then
        printf '%s/prebuilt/macos-%s\n' "$OPENSSL_DIR" "$ARCH"
    elif [[ "$OS" == "Linux" ]]; then
        printf '%s/prebuilt/linux-%s\n' "$OPENSSL_DIR" "$ARCH"
    else
        printf '%s\n' "$OPENSSL_DIR"
    fi
}

sync_output_file() {
    local src="$1"
    local dst="$2"
    local tmp="${dst}.tmp.$$"
    mkdir -p "$(dirname "${dst}")"
    cp "${src}" "${tmp}"
    mv "${tmp}" "${dst}"
}

sync_headers() {
    local dst="${OUTPUT_DIR}/include"
    if [[ ! -d "${OPENSSL_DIR}/include/openssl" ]]; then
        # Prebuilt-only checkout (e.g. CI): third_party/openssl-*/* is
        # gitignored except prebuilt/, so there is no source header tree to
        # copy from. The committed prebuilt headers under ${dst}/openssl are
        # already canonical — leave them in place instead of deleting them.
        if [[ -d "${dst}/openssl" ]]; then
            return 0
        fi
        echo -e "${RED}Error: no OpenSSL source headers (${OPENSSL_DIR}/include/openssl)" >&2
        echo -e "       and no prebuilt headers (${dst}/openssl)${NC}" >&2
        exit 1
    fi
    rm -rf "${dst}"
    mkdir -p "${dst}"
    cp -R "${OPENSSL_DIR}/include/openssl" "${dst}/openssl"
    find "${dst}/openssl" -name '*.in' -delete
}

clean_openssl_tree() {
    # Keep prebuilt/ intact. It lives inside the OpenSSL source directory and
    # stores completed platform slices. Do not use `find ... -prune ... -delete`
    # here: `-delete` implies `-depth`, so prune is evaluated too late.
    make distclean >/dev/null 2>&1 || make clean >/dev/null 2>&1 || true
    find . \( -name '*.o' -o -name '*.d' -o -name '*.a' -o -name '*.so' -o -name '*.dylib' \) \
        -type f ! -path './prebuilt/*' -delete >/dev/null 2>&1 || true
    rm -f Makefile Makefile.in configdata.pm builddata.pm \
        libcrypto.pc libssl.pc openssl.pc \
        OpenSSLConfig.cmake OpenSSLConfigVersion.cmake
}

if [[ "$OS" == "Darwin" ]]; then
    REPO_MACOS_DEPLOYMENT_TARGET="$(read_repo_macos_target)"
    REQUESTED_MACOS_TARGET="${OPENSSL_MACOS_DEPLOYMENT_TARGET:-${CMAKE_OSX_DEPLOYMENT_TARGET:-$REPO_MACOS_DEPLOYMENT_TARGET}}"
    if [[ "$(version_equal "$REQUESTED_MACOS_TARGET" "$REPO_MACOS_DEPLOYMENT_TARGET")" != "1" ]]; then
        echo -e "${RED}Error: repo policy requires macOS ${REPO_MACOS_DEPLOYMENT_TARGET}, but requested ${REQUESTED_MACOS_TARGET}${NC}" >&2
        echo -e "${RED}   Re-run with OPENSSL_MACOS_DEPLOYMENT_TARGET=${REPO_MACOS_DEPLOYMENT_TARGET}${NC}" >&2
        exit 1
    fi
    OPENSSL_MACOS_DEPLOYMENT_TARGET="$REPO_MACOS_DEPLOYMENT_TARGET"
else
    OPENSSL_MACOS_DEPLOYMENT_TARGET="${OPENSSL_MACOS_DEPLOYMENT_TARGET:-${CMAKE_OSX_DEPLOYMENT_TARGET:-}}"
fi

OUTPUT_DIR="$(default_output_dir)"
BUILD_METADATA_FILE="${OUTPUT_DIR}/.dinero-build-meta"
mkdir -p "${OUTPUT_DIR}"

if [[ "$OUTPUT_DIR" != "$OPENSSL_DIR" ]] &&
   [[ ! -f "${OUTPUT_DIR}/libcrypto.a" || ! -f "${OUTPUT_DIR}/libssl.a" ]] &&
   [[ -f "${OPENSSL_DIR}/libcrypto.a" && -f "${OPENSSL_DIR}/libssl.a" ]]; then
    LEGACY_TARGET=""
    if [[ "$OS" == "Darwin" ]]; then
        LEGACY_TARGET="$(detect_archive_target "${OPENSSL_DIR}/libcrypto.a")"
        if [[ -z "${LEGACY_TARGET}" ]]; then
            LEGACY_TARGET="$(awk -F= '/^MACOSX_DEPLOYMENT_TARGET=/{print $2; exit}' "${OPENSSL_DIR}/.dinero-build-meta" 2>/dev/null || true)"
        fi
    fi

    if [[ "$OS" != "Darwin" || -z "${LEGACY_TARGET}" || "$(version_equal "${LEGACY_TARGET}" "${OPENSSL_MACOS_DEPLOYMENT_TARGET}")" == "1" ]]; then
        echo -e "${YELLOW}Migrating legacy vendored OpenSSL artifacts into ${OUTPUT_DIR}${NC}"
        sync_output_file "${OPENSSL_DIR}/libcrypto.a" "${OUTPUT_DIR}/libcrypto.a"
        sync_output_file "${OPENSSL_DIR}/libssl.a" "${OUTPUT_DIR}/libssl.a"
        if [[ -f "${OPENSSL_DIR}/.dinero-build-meta" ]]; then
            sync_output_file "${OPENSSL_DIR}/.dinero-build-meta" "${BUILD_METADATA_FILE}"
        fi
    fi
fi

echo -e "${BLUE}Platform:${NC} $OS $ARCH"
echo -e "${BLUE}Source:${NC} $OPENSSL_DIR"
echo -e "${BLUE}Output:${NC} $OUTPUT_DIR"
if [ "$OS" = "Darwin" ]; then
    echo -e "${BLUE}macOS min deployment target:${NC} ${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
fi
echo ""

# Check if already built
if [ -f "$OUTPUT_DIR/libcrypto.a" ] && [ -f "$OUTPUT_DIR/libssl.a" ]; then
    EXISTING_TARGET=""
    EXISTING_TARGET_METADATA=""
    EXISTING_TARGET_ARCHIVE=""
    EXISTING_OS="$(read_metadata_field OS)"
    EXISTING_ARCH="$(read_metadata_field ARCH)"
    if [[ -n "$EXISTING_OS" && "$EXISTING_OS" != "$OS" ]]; then
        echo -e "${YELLOW}Warning: existing OpenSSL OS (${EXISTING_OS}) does not match host OS (${OS})${NC}"
        OPENSSL_REBUILD=1
    fi
    if [[ -n "$EXISTING_ARCH" && "$EXISTING_ARCH" != "$ARCH" ]]; then
        echo -e "${YELLOW}Warning: existing OpenSSL arch (${EXISTING_ARCH}) does not match host arch (${ARCH})${NC}"
        OPENSSL_REBUILD=1
    fi
    if [[ "$OS" == "Darwin" ]]; then
        EXISTING_TARGET_METADATA="$(read_metadata_target)"
        EXISTING_TARGET_ARCHIVE="$(detect_archive_target "$OUTPUT_DIR/libcrypto.a")"
        if [[ -n "$EXISTING_TARGET_METADATA" && -n "$EXISTING_TARGET_ARCHIVE" && "$(version_equal "$EXISTING_TARGET_METADATA" "$EXISTING_TARGET_ARCHIVE")" != "1" ]]; then
            echo -e "${YELLOW}Warning: vendored OpenSSL metadata target (${EXISTING_TARGET_METADATA}) does not match archive target (${EXISTING_TARGET_ARCHIVE})${NC}"
            OPENSSL_REBUILD=1
        fi
        if [[ -n "$EXISTING_TARGET_ARCHIVE" ]]; then
            EXISTING_TARGET="$EXISTING_TARGET_ARCHIVE"
        else
            EXISTING_TARGET="$EXISTING_TARGET_METADATA"
        fi
    fi

    echo -e "${YELLOW}Warning: OpenSSL libraries already exist:${NC}"
    echo "  libcrypto.a: $(du -h "$OUTPUT_DIR/libcrypto.a" | cut -f1)"
    echo "  libssl.a: $(du -h "$OUTPUT_DIR/libssl.a" | cut -f1)"
    if [[ -n "$EXISTING_TARGET" ]]; then
        echo "  current macOS target: ${EXISTING_TARGET}"
    fi
    echo ""
    if [[ "$OS" == "Darwin" && -n "$EXISTING_TARGET" && "$(version_equal "$EXISTING_TARGET" "$OPENSSL_MACOS_DEPLOYMENT_TARGET")" != "1" ]]; then
        echo -e "${YELLOW}Warning: existing OpenSSL target (${EXISTING_TARGET}) does not match requested target (${OPENSSL_MACOS_DEPLOYMENT_TARGET})${NC}"
        OPENSSL_REBUILD=1
    fi
    if [[ "${OPENSSL_REBUILD}" == "1" ]]; then
        echo -e "${YELLOW}OPENSSL_REBUILD=1 set, rebuilding without prompt${NC}"
    else
        if [[ ! -t 0 ]]; then
            sync_headers
            echo -e "${GREEN}Using existing OpenSSL libraries${NC}"
            exit 0
        fi
        read -p "Rebuild anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            sync_headers
            echo -e "${GREEN}Using existing OpenSSL libraries${NC}"
            exit 0
        fi
    fi
    echo -e "${YELLOW}Cleaning previous build...${NC}"
    cd "$OPENSSL_DIR"
    clean_openssl_tree
fi

# Change to OpenSSL directory
cd "$OPENSSL_DIR"
clean_openssl_tree

# Configure based on platform
echo -e "${BLUE}Configuring OpenSSL...${NC}"

CONFIGURE_FLAGS=(
    no-shared        # Static libraries only
    no-tests         # Skip test suite
    no-apps          # Don't build openssl binary
    enable-ec        # Elliptic curve support
    enable-ecdh      # ECDH support
    enable-ecdsa     # ECDSA support
)

if [ "$OS" = "Darwin" ]; then
    export MACOSX_DEPLOYMENT_TARGET="${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
    export CFLAGS="${CFLAGS:-} -mmacosx-version-min=${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
    export CPPFLAGS="${CPPFLAGS:-} -mmacosx-version-min=${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
    export LDFLAGS="${LDFLAGS:-} -mmacosx-version-min=${OPENSSL_MACOS_DEPLOYMENT_TARGET}"

    # macOS configuration
    if [ "$ARCH" = "arm64" ]; then
        TARGET="darwin64-arm64-cc"
    else
        TARGET="darwin64-x86_64-cc"
    fi
    echo -e "${BLUE}macOS target:${NC} $TARGET"
    echo -e "${BLUE}OpenSSL compile flags:${NC} CFLAGS='${CFLAGS}' LDFLAGS='${LDFLAGS}'"
    ./Configure "$TARGET" "${CONFIGURE_FLAGS[@]}"
elif [ "$OS" = "Linux" ]; then
    # Linux configuration
    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
        TARGET="linux-aarch64"
    else
        TARGET="linux-x86_64"
    fi
    echo -e "${BLUE}Linux target:${NC} $TARGET"
    ./Configure "$TARGET" "${CONFIGURE_FLAGS[@]}"
else
    echo -e "${RED}Unsupported platform: $OS${NC}"
    exit 1
fi

# Build with all available cores
echo -e "${BLUE}Building OpenSSL (this may take a few minutes)...${NC}"

if [ "$OS" = "Darwin" ]; then
    CORES=$(sysctl -n hw.ncpu)
else
    CORES=$(nproc)
fi

echo -e "${BLUE}Using $CORES CPU cores${NC}"
make -j"$CORES"

# Verify libraries were created
echo ""
echo -e "${BLUE}Verifying build...${NC}"

if [ ! -f "libcrypto.a" ] || [ ! -f "libssl.a" ]; then
    echo -e "${RED}Build failed: Libraries not found${NC}"
    exit 1
fi

# Display library sizes
CRYPTO_SIZE=$(du -h libcrypto.a | cut -f1)
SSL_SIZE=$(du -h libssl.a | cut -f1)

{
    echo "OS=${OS}"
    echo "ARCH=${ARCH}"
    echo "OPENSSL_VERSION=${OPENSSL_VERSION}"
    if [[ "$OS" == "Darwin" ]]; then
        echo "MACOSX_DEPLOYMENT_TARGET=${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
    fi
    echo "BUILT_AT_UTC=$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
} > "${BUILD_METADATA_FILE}.tmp.$$"
mv "${BUILD_METADATA_FILE}.tmp.$$" "$BUILD_METADATA_FILE"

if [[ "$OUTPUT_DIR" != "$OPENSSL_DIR" ]]; then
    sync_output_file "libcrypto.a" "${OUTPUT_DIR}/libcrypto.a"
    sync_output_file "libssl.a" "${OUTPUT_DIR}/libssl.a"
fi
sync_headers

echo -e "${GREEN}OpenSSL built successfully!${NC}"
echo ""
echo -e "${GREEN}Libraries created:${NC}"
echo "  libcrypto.a: $(du -h "${OUTPUT_DIR}/libcrypto.a" | cut -f1)"
echo "  libssl.a: $(du -h "${OUTPUT_DIR}/libssl.a" | cut -f1)"
if [[ "$OS" == "Darwin" ]]; then
    echo "  macOS target: ${OPENSSL_MACOS_DEPLOYMENT_TARGET}"
fi
echo ""

# Optional: Run basic test
echo -e "${BLUE}Running basic smoke test...${NC}"
if ./apps/openssl version 2>/dev/null; then
    VERSION=$(./apps/openssl version)
    echo -e "${GREEN}  $VERSION${NC}"
else
    echo -e "${YELLOW}  openssl binary test skipped (apps disabled)${NC}"
fi

echo ""
echo -e "${GREEN}--------------------------------------------------------${NC}"
echo -e "${GREEN}Vendored OpenSSL ready for DineroCoin builds${NC}"
echo -e "${GREEN}--------------------------------------------------------${NC}"
echo ""
echo -e "You can now build DineroCoin with:"
echo -e "  ${BLUE}cmake -B build -S . -DDINERO_VENDORED_OPENSSL_DIR=\"${OUTPUT_DIR}\" -DDINERO_VENDORED_OPENSSL_SOURCE_DIR=\"${OPENSSL_DIR}\"${NC}"
echo -e "  ${BLUE}cmake --build build -j${CORES}${NC}"
echo ""
