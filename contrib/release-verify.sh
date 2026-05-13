#!/usr/bin/env bash
# DineroCoin Release Verification Script
# Phase Z.4: Automated Release Checklist Verification
#
# Purpose: Verify release readiness before tagging
# Usage: ./contrib/release-verify.sh [version]
#
# Example: ./contrib/release-verify.sh v1.0.0

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Track test results
PASSED=0
FAILED=0
WARNINGS=0

# Version to verify
VERSION="${1:-unknown}"

print_header() {
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}$1${NC}"
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_section() {
    echo -e "\n${BLUE}## $1${NC}\n"
}

check_pass() {
    echo -e "${GREEN}✅${NC} $1"
    ((PASSED++))
}

check_fail() {
    echo -e "${RED}✗${NC} $1"
    ((FAILED++))
}

check_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
    ((WARNINGS++))
}

check_info() {
    echo -e "ℹ️  $1"
}

# ============================================================================
# Phase Z.1: Build Reproducibility
# ============================================================================

verify_build_reproducibility() {
    print_section "Phase Z.1: Build Reproducibility"

    # Check for deterministic build script
    if [ -f "contrib/build-deterministic.sh" ]; then
        check_pass "Deterministic build script exists"
    else
        check_fail "Deterministic build script not found"
    fi

    # Check for REPRODUCIBLE_BUILDS.md
    if [ -f "docs/REPRODUCIBLE_BUILDS.md" ]; then
        check_pass "Reproducible builds documentation exists"
    else
        check_fail "REPRODUCIBLE_BUILDS.md not found"
    fi

    # Check for DEPENDENCIES.md
    if [ -f "docs/DEPENDENCIES.md" ]; then
        check_pass "Dependencies documentation exists"

        # Check for pinned versions
        if grep -q "RocksDB.*8\.11\.3" docs/DEPENDENCIES.md; then
            check_pass "RocksDB version pinned (8.11.3)"
        else
            check_warn "RocksDB version not explicitly pinned to 8.11.3"
        fi

        if grep -q "SQLite.*3\.48\.0" docs/DEPENDENCIES.md; then
            check_pass "SQLite version pinned (3.48.0)"
        else
            check_warn "SQLite version not explicitly pinned to 3.48.0"
        fi

        if grep -q "OpenSSL.*3\.3\.2" docs/DEPENDENCIES.md; then
            check_pass "OpenSSL version pinned (3.3.2)"
        else
            check_warn "OpenSSL version not explicitly pinned to 3.3.2"
        fi
    else
        check_fail "DEPENDENCIES.md not found"
    fi

    # Check for vendored dependencies
    local vendored_count=0
    [ -d "third_party/argon2" ] && ((vendored_count++))
    [ -d "third_party/jsoncpp" ] && ((vendored_count++))
    [ -d "third_party/secp256k1-zkp" ] && ((vendored_count++))
    [ -d "third_party/sqlite-amalgamation-3480000" ] && ((vendored_count++))

    if [ "$vendored_count" -ge 3 ]; then
        check_pass "Vendored dependencies present ($vendored_count/4)"
    else
        check_warn "Some vendored dependencies missing ($vendored_count/4)"
    fi

    # Check compiler version requirements
    if command -v gcc-11 &> /dev/null; then
        check_pass "GCC 11 available"
    else
        check_warn "GCC 11 not found (required for Linux deterministic builds)"
    fi

    if command -v clang &> /dev/null; then
        CLANG_VERSION=$(clang --version | head -n1)
        check_info "Clang version: $CLANG_VERSION"
    fi

    # Check CMake version
    if command -v cmake &> /dev/null; then
        CMAKE_VERSION=$(cmake --version | grep -oP 'cmake version \K[0-9.]+' | head -1)
        check_info "CMake version: $CMAKE_VERSION"

        CMAKE_MAJOR=$(echo "$CMAKE_VERSION" | cut -d. -f1)
        CMAKE_MINOR=$(echo "$CMAKE_VERSION" | cut -d. -f2)

        if [ "$CMAKE_MAJOR" -ge 3 ] && [ "$CMAKE_MINOR" -ge 26 ]; then
            check_pass "CMake version >= 3.26 ($CMAKE_VERSION)"
        else
            check_fail "CMake version < 3.26 ($CMAKE_VERSION)"
        fi
    else
        check_fail "CMake not found"
    fi
}

# ============================================================================
# Phase Z.2: Configuration Compatibility
# ============================================================================

verify_configuration_compatibility() {
    print_section "Phase Z.2: Configuration Compatibility"

    # Check for configuration documentation
    if [ -f "docs/CONFIGURATION.md" ]; then
        check_pass "Configuration documentation exists"

        # Check for config_version
        if grep -q "config_version=1" docs/CONFIGURATION.md; then
            check_pass "config_version=1 documented"
        else
            check_warn "config_version not found in CONFIGURATION.md"
        fi

        # Check for stability levels
        if grep -q "STABLE\|EXPERIMENTAL\|DEPRECATED" docs/CONFIGURATION.md; then
            check_pass "Stability levels documented"
        else
            check_warn "Stability levels not found"
        fi
    else
        check_fail "CONFIGURATION.md not found"
    fi

    # Check for migration guide
    if [ -f "docs/CONFIG_MIGRATION.md" ]; then
        check_pass "Configuration migration guide exists"
    else
        check_fail "CONFIG_MIGRATION.md not found"
    fi

    # Check for example config
    if [ -f "contrib/dinero.conf.example" ]; then
        check_pass "Example configuration file exists"

        # Check for essential config options
        if grep -q "rpcuser\|rpcpassword" contrib/dinero.conf.example; then
            check_pass "RPC authentication in example config"
        else
            check_warn "RPC authentication not in example config"
        fi

        if grep -q "config_version" contrib/dinero.conf.example; then
            check_pass "config_version in example config"
        else
            check_warn "config_version not in example config"
        fi
    else
        check_fail "dinero.conf.example not found"
    fi

    # Check ConfigService header
    if [ -f "include/daemon/services/config_service.h" ]; then
        check_pass "ConfigService header exists"
    else
        check_warn "ConfigService header not found"
    fi
}

# ============================================================================
# Phase Z.3: RPC API Compatibility
# ============================================================================

verify_rpc_compatibility() {
    print_section "Phase Z.3: RPC API Compatibility"

    # Check for RPC compatibility documentation
    if [ -f "docs/RPC_COMPATIBILITY.md" ]; then
        check_pass "RPC compatibility documentation exists"

        # Check for api_version
        if grep -q "api_version.*1" docs/RPC_COMPATIBILITY.md; then
            check_pass "api_version=1 documented"
        else
            check_warn "api_version not found in RPC_COMPATIBILITY.md"
        fi

        # Check for stability levels
        if grep -q "STABLE.*EXPERIMENTAL.*DEPRECATED.*INTERNAL" docs/RPC_COMPATIBILITY.md; then
            check_pass "RPC stability levels documented"
        else
            check_warn "RPC stability levels incomplete"
        fi

        # Check for deprecation policy
        if grep -q "deprecation.*cycle\|1 major version" docs/RPC_COMPATIBILITY.md; then
            check_pass "Deprecation cycle documented"
        else
            check_warn "Deprecation cycle not clearly documented"
        fi
    else
        check_fail "RPC_COMPATIBILITY.md not found"
    fi

    # Check for RPC examples script
    if [ -f "contrib/rpc-examples.sh" ] && [ -x "contrib/rpc-examples.sh" ]; then
        check_pass "RPC examples script exists and is executable"
    else
        check_fail "rpc-examples.sh not found or not executable"
    fi

    # Check for RPC registry
    if [ -f "include/dinero/core/rpc/rpc_registry.h" ]; then
        check_pass "RPC registry header exists"
    else
        check_warn "RPC registry header not found"
    fi

    # Count RPC handler files
    RPC_HANDLER_COUNT=$(find . -name "*_rpc_handlers.cpp" 2>/dev/null | wc -l)
    if [ "$RPC_HANDLER_COUNT" -gt 0 ]; then
        check_pass "RPC handler files found ($RPC_HANDLER_COUNT files)"
    else
        check_warn "No RPC handler files found"
    fi

    # Check for OpenRPC support
    if [ -f "include/rpc/methods_openrpc.h" ]; then
        check_pass "OpenRPC support headers exist"
    else
        check_warn "OpenRPC headers not found"
    fi
}

# ============================================================================
# Consensus Safety
# ============================================================================

verify_consensus_safety() {
    print_section "Consensus Safety"

    # Check for consensus code
    if [ -d "src/consensus" ] || [ -d "include/consensus" ]; then
        check_pass "Consensus directory exists"

        # Check for floating point in consensus code (should be none)
        if grep -r "float\|double" src/consensus/ include/consensus/ 2>/dev/null | grep -v "comment\|//" | grep -q "float\|double"; then
            check_fail "Floating point found in consensus code (CRITICAL)"
        else
            check_pass "No floating point in consensus code"
        fi
    else
        check_warn "Consensus directory not found"
    fi

    # Check for unsafe C functions
    UNSAFE_FUNCS=$(grep -r "strcpy\|sprintf\|gets" src/ 2>/dev/null | grep -v "snprintf\|comment\|//" | wc -l)
    if [ "$UNSAFE_FUNCS" -eq 0 ]; then
        check_pass "No unsafe C string functions found"
    else
        check_warn "Unsafe C functions found ($UNSAFE_FUNCS occurrences)"
    fi

    # Check for consensus tests
    if [ -f "tests/test_consensus.cpp" ] || [ -f "test/test_consensus.cpp" ]; then
        check_pass "Consensus tests exist"
    else
        check_warn "Consensus tests not found"
    fi
}

# ============================================================================
# Documentation Completeness
# ============================================================================

verify_documentation() {
    print_section "Documentation Completeness"

    # Core documentation files
    local docs=(
        "README.md"
        "docs/CONFIGURATION.md"
        "docs/CONFIG_MIGRATION.md"
        "docs/RPC_COMPATIBILITY.md"
        "docs/RPC_API.md"
        "docs/REPRODUCIBLE_BUILDS.md"
        "docs/DEPENDENCIES.md"
        "docs/RELEASE_CHECKLIST_V1.md"
    )

    local found=0
    for doc in "${docs[@]}"; do
        if [ -f "$doc" ]; then
            ((found++))
        fi
    done

    if [ "$found" -eq "${#docs[@]}" ]; then
        check_pass "All core documentation files exist ($found/${#docs[@]})"
    else
        check_warn "Some documentation files missing ($found/${#docs[@]})"

        for doc in "${docs[@]}"; do
            if [ ! -f "$doc" ]; then
                check_info "Missing: $doc"
            fi
        done
    fi

    # Check for CHANGELOG.md
    if [ -f "CHANGELOG.md" ]; then
        check_pass "CHANGELOG.md exists"
    else
        check_warn "CHANGELOG.md not found"
    fi

    # Check for SECURITY.md
    if [ -f "SECURITY.md" ]; then
        check_pass "SECURITY.md exists"
    else
        check_warn "SECURITY.md not found"
    fi

    # Check for CONTRIBUTING.md
    if [ -f "CONTRIBUTING.md" ]; then
        check_pass "CONTRIBUTING.md exists"
    else
        check_warn "CONTRIBUTING.md not found"
    fi
}

# ============================================================================
# Git and Version Control
# ============================================================================

verify_git_status() {
    print_section "Git Status"

    # Check if in git repository
    if ! git rev-parse --git-dir &> /dev/null; then
        check_fail "Not in a git repository"
        return
    fi

    check_pass "In git repository"

    # Check for uncommitted changes
    if git diff-index --quiet HEAD --; then
        check_pass "No uncommitted changes"
    else
        check_warn "Uncommitted changes detected (commit before release)"
    fi

    # Check current branch
    CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
    check_info "Current branch: $CURRENT_BRANCH"

    # Check if at a tag
    if git describe --exact-match &> /dev/null; then
        GIT_TAG=$(git describe --exact-match)
        check_info "At git tag: $GIT_TAG"
    else
        check_info "Not at a git tag (create tag for release)"
    fi

    # Check for Phase Z tags
    PHASE_Z_TAGS=$(git tag -l "phase-z.*" | wc -l)
    if [ "$PHASE_Z_TAGS" -ge 4 ]; then
        check_pass "Phase Z tags exist ($PHASE_Z_TAGS tags)"
    else
        check_warn "Some Phase Z tags missing ($PHASE_Z_TAGS/4)"
    fi

    # List Phase Z tags
    if [ "$PHASE_Z_TAGS" -gt 0 ]; then
        check_info "Phase Z tags: $(git tag -l 'phase-z.*' | tr '\n' ' ')"
    fi
}

# ============================================================================
# Build System
# ============================================================================

verify_build_system() {
    print_section "Build System"

    # Check for CMakeLists.txt
    if [ -f "CMakeLists.txt" ]; then
        check_pass "CMakeLists.txt exists"
    else
        check_fail "CMakeLists.txt not found"
    fi

    # Check for build directory
    if [ -d "build" ] || [ -d "build-deterministic" ]; then
        check_pass "Build directory exists"
    else
        check_info "No build directory (run cmake to create)"
    fi

    # Check for test executables
    if [ -d "build" ] && [ -d "build/bin" ]; then
        TEST_COUNT=$(find build/bin -name "test_*" -type f 2>/dev/null | wc -l)
        if [ "$TEST_COUNT" -gt 0 ]; then
            check_pass "Test executables found ($TEST_COUNT tests)"
        else
            check_warn "No test executables found"
        fi
    fi
}

# ============================================================================
# Security Checks
# ============================================================================

verify_security() {
    print_section "Security"

    # Check for .cookie file handling
    if grep -r "\.cookie" src/ include/ 2>/dev/null | grep -q "0600\|chmod"; then
        check_pass "Cookie file permissions checked in code"
    else
        check_warn "Cookie file permission handling not verified"
    fi

    # Check for SQL injection protection (prepared statements)
    if grep -r "sqlite3_prepare\|SQLITE_PREPARE" src/ include/ 2>/dev/null | grep -q "sqlite3_prepare"; then
        check_pass "SQLite prepared statements used"
    else
        check_warn "SQLite prepared statements not verified"
    fi

    # Check for command injection vectors
    SYSTEM_CALLS=$(grep -r "system\|popen\|exec" src/ 2>/dev/null | grep -v "comment\|//\|ExecutionContext" | wc -l)
    if [ "$SYSTEM_CALLS" -eq 0 ]; then
        check_pass "No system() calls found"
    else
        check_warn "system/popen/exec calls found ($SYSTEM_CALLS occurrences - review carefully)"
    fi

    # Check for Argon2 (wallet encryption)
    if [ -d "third_party/argon2" ]; then
        check_pass "Argon2 library vendored (wallet encryption)"
    else
        check_warn "Argon2 library not found"
    fi
}

# ============================================================================
# Final Summary
# ============================================================================

print_summary() {
    print_header "Release Verification Summary"

    echo ""
    echo "Version: $VERSION"
    echo ""
    echo -e "${GREEN}Passed:   $PASSED${NC}"
    echo -e "${YELLOW}Warnings: $WARNINGS${NC}"
    echo -e "${RED}Failed:   $FAILED${NC}"
    echo ""

    if [ "$FAILED" -eq 0 ] && [ "$WARNINGS" -eq 0 ]; then
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${GREEN}🚀 DineroCoin $VERSION READY FOR RELEASE${NC}"
        echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo ""
        echo "Next steps:"
        echo "1. Review docs/RELEASE_CHECKLIST_V1.md for manual verification items"
        echo "2. Create git tag: git tag -a $VERSION -m \"Release $VERSION\""
        echo "3. Build deterministic binaries: ./contrib/build-deterministic.sh"
        echo "4. Generate SHA256 checksums"
        echo "5. Sign release artifacts with GPG"
        echo "6. Push tag: git push origin $VERSION"
        echo "7. Publish GitHub release with binaries"
        echo "8. Announce to community"
        echo ""
        return 0
    elif [ "$FAILED" -eq 0 ]; then
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${YELLOW}⚠ DineroCoin $VERSION - Warnings Found${NC}"
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo ""
        echo "Review warnings before proceeding with release."
        echo "See docs/RELEASE_CHECKLIST_V1.md for complete checklist."
        echo ""
        return 1
    else
        echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${RED}✗ DineroCoin $VERSION - NOT READY${NC}"
        echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo ""
        echo "Fix failing checks before release."
        echo "See docs/RELEASE_CHECKLIST_V1.md for complete checklist."
        echo ""
        return 1
    fi
}

# ============================================================================
# Main Execution
# ============================================================================

main() {
    print_header "DineroCoin Release Verification - Phase Z.4"
    echo ""
    echo "Version: $VERSION"
    echo "Date: $(date)"
    echo ""

    verify_build_reproducibility
    verify_configuration_compatibility
    verify_rpc_compatibility
    verify_consensus_safety
    verify_documentation
    verify_git_status
    verify_build_system
    verify_security

    print_summary
}

# Run main function
main
exit $?
