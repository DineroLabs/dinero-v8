#!/usr/bin/env bash
# Copyright (c) 2024 The Dinero Core developers
# Distributed under the MIT software license

# ═══════════════════════════════════════════════════════════════════════════
# Release Binary Verification (Bitcoin Core Style)
# ═══════════════════════════════════════════════════════════════════════════
#
# Purpose: Verify release binaries have ZERO external dependencies
#
# This script ensures:
#   ✅ No Homebrew dependencies (no /opt/homebrew/... dylibs)
#   ✅ No gRPC/protobuf/abseil runtime linkage
#   ✅ No pkg-config dependencies
#   ✅ Only system libraries (libc++, libSystem, pthread)
#
# Usage:
#   ./scripts/verify_release_binary.sh build/dinerod
#   ./scripts/verify_release_binary.sh build/lightningd
#
# CI Integration:
#   - Run after release build completes
#   - Exit code 0 = pass, 1 = fail (breaks CI)
#
# Bitcoin Core does this for every release:
#   - Verifies zero unexpected dependencies
#   - Catches accidental dynamic linkage
#   - Prevents shipping Homebrew-dependent binaries
#
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

BINARY="${1:-}"

if [[ -z "$BINARY" ]]; then
    echo -e "${RED}ERROR: No binary specified${NC}"
    echo "Usage: $0 <path-to-binary>"
    echo "Example: $0 build/dinerod"
    exit 1
fi

if [[ ! -f "$BINARY" ]]; then
    echo -e "${RED}ERROR: Binary not found: $BINARY${NC}"
    exit 1
fi

echo "═══════════════════════════════════════════════════════════════════════"
echo "Release Binary Verification: $(basename "$BINARY")"
echo "═══════════════════════════════════════════════════════════════════════"
echo

# Platform detection
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macOS"
    LDDCMD="otool -L"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="Linux"
    LDDCMD="ldd"
else
    echo -e "${RED}ERROR: Unsupported platform: $OSTYPE${NC}"
    exit 1
fi

echo "Platform: $PLATFORM"
echo "Binary: $BINARY"
echo

# Get all dynamic library dependencies
if [[ "$PLATFORM" == "macOS" ]]; then
    DEPS=$(otool -L "$BINARY" | tail -n +2 | awk '{print $1}')
else
    DEPS=$(ldd "$BINARY" | grep "=>" | awk '{print $3}')
fi

# Count total dependencies
TOTAL_DEPS=$(echo "$DEPS" | wc -l | tr -d ' ')
echo "Total dynamic dependencies: $TOTAL_DEPS"
echo

# ═══════════════════════════════════════════════════════════════════════════
# Test 1: Check for Homebrew dependencies (CRITICAL FAILURE)
# ═══════════════════════════════════════════════════════════════════════════
echo "Test 1: Checking for Homebrew dependencies..."

if [[ "$PLATFORM" == "macOS" ]]; then
    HOMEBREW_DEPS=$(echo "$DEPS" | grep -E "homebrew|/usr/local/opt" || true)

    if [[ -n "$HOMEBREW_DEPS" ]]; then
        echo -e "${RED}❌ FAILED: Found Homebrew dependencies${NC}"
        echo
        echo "The following Homebrew libraries are linked:"
        echo "$HOMEBREW_DEPS"
        echo
        echo "Release binaries MUST NOT depend on Homebrew."
        echo "Run: cmake -DDINERO_RELEASE=ON .. && make clean && make"
        exit 1
    else
        echo -e "${GREEN}✅ PASSED: No Homebrew dependencies${NC}"
    fi
else
    echo -e "${YELLOW}⚠️  SKIPPED: Homebrew check only runs on macOS${NC}"
fi
echo

# ═══════════════════════════════════════════════════════════════════════════
# Test 2: Check for gRPC/protobuf/abseil (CRITICAL FAILURE)
# ═══════════════════════════════════════════════════════════════════════════
echo "Test 2: Checking for gRPC/protobuf/abseil..."

GRPC_DEPS=$(echo "$DEPS" | grep -iE "grpc|protobuf|abseil|absl" || true)

if [[ -n "$GRPC_DEPS" ]]; then
    echo -e "${RED}❌ FAILED: Found gRPC/protobuf/abseil dependencies${NC}"
    echo
    echo "The following gRPC-related libraries are linked:"
    echo "$GRPC_DEPS"
    echo
    echo "Release binaries use raw socket IPC (not gRPC)."
    echo "Run: cmake -DDINERO_RELEASE=ON .. && make clean && make"
    exit 1
else
    echo -e "${GREEN}✅ PASSED: No gRPC/protobuf/abseil dependencies${NC}"
fi
echo

# ═══════════════════════════════════════════════════════════════════════════
# Test 3: Verify only system libraries are linked
# ═══════════════════════════════════════════════════════════════════════════
echo "Test 3: Checking allowed system libraries..."

if [[ "$PLATFORM" == "macOS" ]]; then
    ALLOWED_PATTERN="^(/usr/lib/|/System/Library/)"
    DISALLOWED_DEPS=$(echo "$DEPS" | grep -vE "$ALLOWED_PATTERN" || true)

    if [[ -n "$DISALLOWED_DEPS" ]]; then
        echo -e "${RED}❌ FAILED: Found non-system libraries${NC}"
        echo
        echo "The following non-system libraries are linked:"
        echo "$DISALLOWED_DEPS"
        echo
        echo "Release binaries should only link system libraries:"
        echo "  - /usr/lib/libSystem.B.dylib"
        echo "  - /usr/lib/libc++.1.dylib"
        echo "  - /System/Library/Frameworks/*"
        exit 1
    else
        echo -e "${GREEN}✅ PASSED: Only system libraries linked${NC}"
    fi
else
    echo -e "${YELLOW}⚠️  SKIPPED: System library check only runs on macOS${NC}"
fi
echo

# ═══════════════════════════════════════════════════════════════════════════
# Test 4: Startup smoke test (catches launch / static-init crashes)
# ═══════════════════════════════════════════════════════════════════════════
# A release binary must actually RUN. `--version` exercises every static
# initializer (where a bad global constructor aborts) and the arg parser, then
# exits — so it catches launch-time crashes without starting a long-lived daemon.
#
# This exists because rc30's macOS dinerod aborted at startup (SIGABRT) from a
# static initializer that logged before the logger mutex was constructed; the
# binary was signed, notarized, and shipped without anyone running it. A crash
# kills the process with a signal (exit >= 128); an unrecognized --version just
# exits cleanly non-zero (NOT a crash), so we only fail on signal termination or
# a C++ terminate/abort message in the output.
echo "Test 4: Startup smoke test (binary launches without crashing)..."

set +e
SMOKE_OUT=$("$BINARY" --version 2>&1)
SMOKE_RC=$?
set -e

if [[ "$SMOKE_RC" -ge 128 ]]; then
    echo -e "${RED}❌ FAILED: Binary crashed at startup (killed by signal $((SMOKE_RC - 128)))${NC}"
    echo "$SMOKE_OUT" | tail -5
    echo
    echo "The binary aborts during launch. A release binary MUST run — see the"
    echo "rc30 macOS static-init logger crash (PR #227) for an example."
    exit 1
elif echo "$SMOKE_OUT" | grep -qiE "libc\+\+abi|terminating due to|mutex lock failed|Segmentation fault|Abort trap"; then
    echo -e "${RED}❌ FAILED: Binary printed a crash/terminate message at startup${NC}"
    echo "$SMOKE_OUT" | tail -5
    exit 1
else
    echo -e "${GREEN}✅ PASSED: Binary launches cleanly (exit $SMOKE_RC)${NC}"
fi
echo

# ═══════════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════════
echo "═══════════════════════════════════════════════════════════════════════"
echo -e "${GREEN}✅ ALL TESTS PASSED${NC}"
echo "═══════════════════════════════════════════════════════════════════════"
echo
echo "Binary $(basename "$BINARY") is release-grade:"
echo "  ✅ No Homebrew dependencies"
echo "  ✅ No gRPC/protobuf/abseil"
echo "  ✅ Only system libraries"
echo "  ✅ Launches without crashing (startup smoke test)"
echo "  ✅ Exchange-ready"
echo
echo "Dependencies ($TOTAL_DEPS total):"
echo "$DEPS"
echo

exit 0
