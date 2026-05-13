#!/bin/bash
# Dinero CLI Release Build Script
# Creates production-ready releases with signing and verification

set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DIST_DIR="$PROJECT_ROOT/dist"
BUILD_DIR="$PROJECT_ROOT/build-release"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[RELEASE]${NC} $1"; }
success() { echo -e "${GREEN}✅ $1${NC}"; }
warning() { echo -e "${YELLOW}⚠️  $1${NC}"; }
error() { echo -e "${RED}❌ $1${NC}"; exit 1; }

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."
    
    command -v git >/dev/null || error "git not found"
    command -v cmake >/dev/null || error "cmake not found"
    command -v make >/dev/null || error "make not found"
    
    # Check if we're in a git repo
    git rev-parse --git-dir >/dev/null 2>&1 || error "Not in a git repository"
    
    # Check for uncommitted changes
    if ! git diff-index --quiet HEAD --; then
        warning "Uncommitted changes detected. Consider committing first."
        read -p "Continue anyway? (y/N): " -n 1 -r
        echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 1
    fi
    
    success "Prerequisites OK"
}

# Get version info
get_version() {
    VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo "1.0.0-unknown")
    COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    BUILD_DATE=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    
    log "Version: $VERSION"
    log "Commit: $COMMIT"
    log "Build Date: $BUILD_DATE"
}

# Clean and prepare
prepare_build() {
    log "Preparing build environment..."
    
    # Clean previous builds
    rm -rf "$DIST_DIR" "$BUILD_DIR"
    mkdir -p "$DIST_DIR" "$BUILD_DIR"
    
    success "Build environment prepared"
}

# Build for current platform
build_current_platform() {
    log "Building for current platform..."
    
    cd "$BUILD_DIR"
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCLI_VERSION="$VERSION" \
          "$PROJECT_ROOT"
    
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) dinero-cli
    
    # Test the build
    ./bin/dinero-cli version >/dev/null || error "Built CLI failed version test"
    
    success "Build completed"
}

# Create distribution package
create_package() {
    local platform="$(uname -s)-$(uname -m)"
    local package_name="dinero-cli-$VERSION-$platform"
    local package_dir="$DIST_DIR/$package_name"
    
    log "Creating package: $package_name"
    
    mkdir -p "$package_dir/bin"
    mkdir -p "$package_dir/share/man/man1"
    mkdir -p "$package_dir/share/bash-completion/completions"
    mkdir -p "$package_dir/share/zsh/site-functions"
    mkdir -p "$package_dir/share/fish/vendor_completions.d"
    
    # Copy binary
    cp "$BUILD_DIR/bin/dinero-cli" "$package_dir/bin/"
    
    # Generate and copy man page
    if command -v help2man >/dev/null; then
        help2man -N -n "Dinero RPC client with Core-style aliases" \
                 -o "$package_dir/share/man/man1/dinero-cli.1" \
                 "$BUILD_DIR/bin/dinero-cli" || warning "Man page generation failed"
    else
        warning "help2man not found, skipping man page generation"
    fi
    
    # Copy completions
    cp "$PROJECT_ROOT/scripts/bash_completion.sh" "$package_dir/share/bash-completion/completions/dinero-cli"
    
    # Copy documentation
    cat > "$package_dir/README.md" << 'EOF'
# Dinero CLI

Enterprise-grade RPC client with Bitcoin Core-style aliases.

## Installation

```bash
# Copy binary to PATH
sudo cp bin/dinero-cli /usr/local/bin/

# Install completions (optional)
sudo cp share/bash-completion/completions/dinero-cli /usr/local/etc/bash_completion.d/

# Install man page (optional)
sudo cp share/man/man1/dinero-cli.1 /usr/local/share/man/man1/
```

## Quick Start

```bash
# Get blockchain height
dinero-cli height

# JSON mode for scripts
dinero-cli --json height

# Wallet operations
dinero-cli --wallet=main wallet balance

# Mining
dinero-cli miner start --threads 8
dinero-cli miner status
dinero-cli miner stop

# Send transaction
dinero-cli tx send din1abc123... 10.5 --subtractfee
```

## Documentation

Run `dinero-cli --help` for full command reference.
EOF
    
    # Create tarball
    cd "$DIST_DIR"
    tar -czf "$package_name.tar.gz" "$package_name"
    
    success "Package created: $package_name.tar.gz"
}

# Generate checksums and signatures
create_checksums() {
    log "Generating checksums..."
    
    cd "$DIST_DIR"
    
    # Generate SHA256 checksums
    shasum -a 256 *.tar.gz > SHA256SUMS.txt
    
    # GPG sign if available
    if command -v gpg >/dev/null && gpg --list-secret-keys >/dev/null 2>&1; then
        log "Signing with GPG..."
        gpg --detach-sign --armor SHA256SUMS.txt
        success "GPG signature created"
    else
        warning "GPG not available or no keys found, skipping signature"
    fi
    
    success "Checksums generated"
}

# Run tests
run_tests() {
    log "Running production tests..."
    
    cd "$PROJECT_ROOT"
    ./scripts/test_cli.sh "$BUILD_DIR/bin/dinero-cli" || error "Tests failed"
    
    success "All tests passed"
}

# Main release process
main() {
    log "Starting Dinero CLI release build..."
    echo
    
    check_prerequisites
    get_version
    prepare_build
    build_current_platform
    run_tests
    create_package
    create_checksums
    
    echo
    success "🎉 Release build completed successfully!"
    echo
    echo "📦 Artifacts created in: $DIST_DIR"
    echo "🏷️  Version: $VERSION"
    echo "📅 Build Date: $BUILD_DATE"
    echo
    echo "Files:"
    ls -la "$DIST_DIR"
    echo
    echo "Ready for distribution! 🚀"
}

# Handle script arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [--help]"
        echo "Creates a production release build of dinero-cli"
        exit 0
        ;;
    *)
        main "$@"
        ;;
esac
