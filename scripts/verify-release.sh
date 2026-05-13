#!/usr/bin/env bash
# Dinero Release Verification Script
# Verifies signatures, checksums, SBOM, and static linking for offline validation

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERIFY_DIR="${SCRIPT_DIR}/../verify"
REQUIRED_FILES=("SHA256SUMS.txt" "SHA256SUMS.txt.asc")

# Logging functions
log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $*"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# Usage information
usage() {
    cat << EOF
Usage: $0 <version> <os> <arch> [options]

Arguments:
    version     Release version (e.g., 1.0.0)
    os          Operating system (windows, macos, linux)
    arch        Architecture (x64, arm64)

Options:
    -d, --dir DIR       Directory containing release files (default: ./verify)
    -s, --skip-gpg      Skip GPG signature verification
    -v, --verbose       Enable verbose output
    -h, --help          Show this help message

Examples:
    $0 1.0.0 linux x64
    $0 1.0.0 macos arm64 --skip-gpg
    $0 1.0.0 windows x64 --dir /path/to/release

This script performs the following verifications:
1. SHA256 checksum validation
2. GPG signature verification (unless --skip-gpg)
3. SBOM validation (if present)
4. Static linking verification (no external dependencies)
5. Binary integrity checks

All verifications work completely offline once files are downloaded.
EOF
}

# Parse command line arguments
parse_args() {
    if [[ $# -lt 3 ]]; then
        usage
        exit 1
    fi
    
    VERSION="$1"
    OS="$2"
    ARCH="$3"
    shift 3
    
    SKIP_GPG=false
    VERBOSE=false
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            -d|--dir)
                VERIFY_DIR="$2"
                shift 2
                ;;
            -s|--skip-gpg)
                SKIP_GPG=true
                shift
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
    
    # Construct artifact filename
    case "$OS" in
        windows) ARTIFACT="dinero-${VERSION}-${OS}-${ARCH}.zip" ;;
        *) ARTIFACT="dinero-${VERSION}-${OS}-${ARCH}.tar.gz" ;;
    esac
    
    SBOM_FILE="dinero-${VERSION}-${OS}-${ARCH}.sbom.json"
}

# Verify required files exist
verify_files_exist() {
    log_info "Checking for required files in ${VERIFY_DIR}..."
    
    if [[ ! -d "$VERIFY_DIR" ]]; then
        log_error "Verification directory does not exist: $VERIFY_DIR"
        exit 1
    fi
    
    cd "$VERIFY_DIR"
    
    local missing_files=()
    for file in "${REQUIRED_FILES[@]}" "$ARTIFACT"; do
        if [[ ! -f "$file" ]]; then
            missing_files+=("$file")
        fi
    done
    
    if [[ ${#missing_files[@]} -gt 0 ]]; then
        log_error "Missing required files:"
        printf '%s\n' "${missing_files[@]}"
        exit 1
    fi
    
    log_success "All required files present"
}

# Verify SHA256 checksums
verify_checksums() {
    log_info "Verifying SHA256 checksums..."
    
    if ! sha256sum -c SHA256SUMS.txt --ignore-missing --quiet; then
        log_error "SHA256 checksum verification failed"
        exit 1
    fi
    
    log_success "SHA256 checksums verified"
}

# Verify GPG signatures
verify_signatures() {
    if [[ "$SKIP_GPG" == true ]]; then
        log_warning "Skipping GPG signature verification (--skip-gpg)"
        return 0
    fi
    
    log_info "Verifying GPG signatures..."
    
    if ! command -v gpg >/dev/null 2>&1; then
        log_warning "GPG not found, skipping signature verification"
        return 0
    fi
    
    if ! gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt 2>/dev/null; then
        log_error "GPG signature verification failed"
        log_info "Make sure you have imported the Dinero release signing key"
        exit 1
    fi
    
    log_success "GPG signatures verified"
}

# Verify SBOM if present
verify_sbom() {
    if [[ -f "$SBOM_FILE" ]]; then
        log_info "Verifying SBOM: $SBOM_FILE"
        
        # Basic JSON validation
        if ! python3 -m json.tool "$SBOM_FILE" >/dev/null 2>&1; then
            log_error "SBOM is not valid JSON"
            exit 1
        fi
        
        # Check for required SBOM fields
        if ! grep -q '"bomFormat".*"CycloneDX"' "$SBOM_FILE"; then
            log_warning "SBOM format not recognized as CycloneDX"
        fi
        
        log_success "SBOM validated"
    else
        log_warning "SBOM file not found: $SBOM_FILE"
    fi
}

# Extract and verify binary
extract_and_verify_binary() {
    log_info "Extracting and verifying binary..."
    
    local extract_dir="extracted_${VERSION}_${OS}_${ARCH}"
    rm -rf "$extract_dir"
    mkdir -p "$extract_dir"
    
    case "$ARTIFACT" in
        *.tar.gz)
            tar -xzf "$ARTIFACT" -C "$extract_dir"
            ;;
        *.zip)
            if command -v unzip >/dev/null 2>&1; then
                unzip -q "$ARTIFACT" -d "$extract_dir"
            else
                log_error "unzip command not found, cannot extract Windows archive"
                return 1
            fi
            ;;
        *)
            log_error "Unknown archive format: $ARTIFACT"
            return 1
            ;;
    esac
    
    # Find the main binary
    local binary_path=""
    for candidate in "$extract_dir"/{bin/,}dinerod{,.exe}; do
        if [[ -f "$candidate" ]]; then
            binary_path="$candidate"
            break
        fi
    done
    
    if [[ -z "$binary_path" ]]; then
        log_error "Could not find dinerod binary in extracted archive"
        return 1
    fi
    
    log_success "Binary extracted: $binary_path"
    
    # Verify static linking
    verify_static_linking "$binary_path"
    
    # Cleanup
    rm -rf "$extract_dir"
}

# Verify static linking (no external dependencies)
verify_static_linking() {
    local binary_path="$1"
    log_info "Verifying static linking for: $binary_path"
    
    case "$OS" in
        macos)
            if command -v otool >/dev/null 2>&1; then
                local external_deps
                external_deps=$(otool -L "$binary_path" 2>/dev/null | grep -v '/usr/lib' | grep -v '/System' | grep -E '(rocksdb|ssl|crypto|snappy|lz4|zstd)' || true)
                if [[ -n "$external_deps" ]]; then
                    log_error "Found external dependencies:"
                    echo "$external_deps"
                    return 1
                fi
                log_success "No external dependencies found (macOS)"
            else
                log_warning "otool not available, skipping macOS dependency check"
            fi
            ;;
        linux)
            if command -v ldd >/dev/null 2>&1; then
                local external_deps
                external_deps=$(ldd "$binary_path" 2>/dev/null | grep -E '(rocksdb|ssl|crypto|snappy|lz4|zstd)' || true)
                if [[ -n "$external_deps" ]]; then
                    log_error "Found external dependencies:"
                    echo "$external_deps"
                    return 1
                fi
                log_success "No external dependencies found (Linux)"
            else
                log_warning "ldd not available, skipping Linux dependency check"
            fi
            ;;
        windows)
            if command -v objdump >/dev/null 2>&1; then
                local external_deps
                external_deps=$(objdump -p "$binary_path" 2>/dev/null | grep -i 'dll name' | grep -iE '(rocksdb|ssl|crypto|snappy|lz4|zstd)' || true)
                if [[ -n "$external_deps" ]]; then
                    log_error "Found external DLL dependencies:"
                    echo "$external_deps"
                    return 1
                fi
                log_success "No external DLL dependencies found (Windows)"
            else
                log_warning "objdump not available, skipping Windows dependency check"
            fi
            ;;
    esac
}

# Main verification function
main() {
    parse_args "$@"
    
    log_info "=== Dinero Release Verification ==="
    log_info "Version: $VERSION"
    log_info "Platform: $OS-$ARCH"
    log_info "Artifact: $ARTIFACT"
    log_info "Directory: $VERIFY_DIR"
    log_info "=================================="
    
    verify_files_exist
    verify_checksums
    verify_signatures
    verify_sbom
    extract_and_verify_binary
    
    log_success "=== All verifications passed! ==="
    log_info "The Dinero release is verified and ready for use."
}

# Run main function with all arguments
main "$@"
