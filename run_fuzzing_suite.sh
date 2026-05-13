#!/usr/bin/env bash
#
# Run Dinero Core consensus safety fuzzers.
# Usage: ./run_fuzzing_suite.sh [duration_seconds]
#

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
DURATION="${1:-300}"  # Default: 5 minutes per fuzzer

# Resolve build dir in this order:
# 1) DINERO_FUZZ_BUILD_DIR env override
# 2) Existing build-fuzz
# 3) Existing build_fuzz (legacy)
# 4) New build-fuzz
if [ -n "${DINERO_FUZZ_BUILD_DIR:-}" ]; then
    BUILD_DIR="$DINERO_FUZZ_BUILD_DIR"
elif [ -d "$PROJECT_ROOT/build-fuzz" ]; then
    BUILD_DIR="$PROJECT_ROOT/build-fuzz"
elif [ -d "$PROJECT_ROOT/build_fuzz" ]; then
    BUILD_DIR="$PROJECT_ROOT/build_fuzz"
else
    BUILD_DIR="$PROJECT_ROOT/build-fuzz"
fi

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Dinero Core - Consensus Safety Fuzzing                  ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Build dir: $BUILD_DIR"
echo "Duration: ${DURATION}s per fuzzer"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

FUZZ_CXX=""
FUZZ_CC=""
FUZZ_CMAKE_CXX_FLAGS=""
FUZZ_CMAKE_EXE_LINKER_FLAGS=""
FUZZ_NEEDS_HOMEBREW_LIBCXX=0

detect_fuzz_compiler() {
    if [ -n "${DINERO_FUZZ_CXX:-}" ] && [ -x "${DINERO_FUZZ_CXX}" ]; then
        FUZZ_CXX="${DINERO_FUZZ_CXX}"
    elif [ -x "/opt/homebrew/opt/llvm/bin/clang++" ]; then
        FUZZ_CXX="/opt/homebrew/opt/llvm/bin/clang++"
    elif command -v clang++ >/dev/null 2>&1; then
        FUZZ_CXX="$(command -v clang++)"
    else
        FUZZ_CXX=""
    fi

    if [ -n "${DINERO_FUZZ_CC:-}" ] && [ -x "${DINERO_FUZZ_CC}" ]; then
        FUZZ_CC="${DINERO_FUZZ_CC}"
    elif [ -n "$FUZZ_CXX" ] && [ -x "${FUZZ_CXX%++}" ]; then
        FUZZ_CC="${FUZZ_CXX%++}"
    elif [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then
        FUZZ_CC="/opt/homebrew/opt/llvm/bin/clang"
    elif command -v clang >/dev/null 2>&1; then
        FUZZ_CC="$(command -v clang)"
    else
        FUZZ_CC=""
    fi

    # Homebrew LLVM libFuzzer on macOS may require matching Homebrew libc++.
    # Without this, linking can fail with undefined std::__1::__hash_memory.
    if [[ "$FUZZ_CXX" == /opt/homebrew/*/clang++ ]] || [[ "$FUZZ_CXX" == /opt/homebrew/opt/llvm/bin/clang++ ]]; then
        FUZZ_NEEDS_HOMEBREW_LIBCXX=1
        FUZZ_CMAKE_CXX_FLAGS="-stdlib=libc++"
        FUZZ_CMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++"
    else
        FUZZ_NEEDS_HOMEBREW_LIBCXX=0
        FUZZ_CMAKE_CXX_FLAGS=""
        FUZZ_CMAKE_EXE_LINKER_FLAGS=""
    fi

    # Allow explicit overrides.
    if [ -n "${DINERO_FUZZ_CXXFLAGS:-}" ]; then
        FUZZ_CMAKE_CXX_FLAGS="${DINERO_FUZZ_CXXFLAGS}"
    fi
    if [ -n "${DINERO_FUZZ_LDFLAGS:-}" ]; then
        FUZZ_CMAKE_EXE_LINKER_FLAGS="${DINERO_FUZZ_LDFLAGS}"
    fi
}

compiler_supports_libfuzzer() {
    local cxx="$1"
    local tmp_bin=""
    tmp_bin="$(mktemp /tmp/dinero-fuzz-probe.XXXXXX)"
    rm -f "$tmp_bin"
    echo 'int main(){return 0;}' | "$cxx" -x c++ - -fsanitize=fuzzer \
        ${FUZZ_CMAKE_CXX_FLAGS:+$FUZZ_CMAKE_CXX_FLAGS} \
        ${FUZZ_CMAKE_EXE_LINKER_FLAGS:+$FUZZ_CMAKE_EXE_LINKER_FLAGS} \
        -o "$tmp_bin" >/dev/null 2>&1
    local status=$?
    rm -f "$tmp_bin"
    return $status
}

ensure_fuzz_build() {
    local cache_file="$BUILD_DIR/CMakeCache.txt"
    local cxx_re=""
    local needs_reconfigure=0
    local cmake_args=()
    local core_targets=(
        fuzz_premine
        fuzz_consensus_limits
        fuzz_tx_deserialize
        fuzz_script_limits
    )
    local t=""

    detect_fuzz_compiler

    if [ -z "$FUZZ_CXX" ] || [ -z "$FUZZ_CC" ]; then
        echo -e "${RED}Error: clang/clang++ not found${NC}"
        echo "libFuzzer requires a Clang toolchain"
        exit 1
    fi

    if ! compiler_supports_libfuzzer "$FUZZ_CXX"; then
        echo -e "${RED}Error: selected compiler does not link with -fsanitize=fuzzer${NC}"
        echo "Selected: $FUZZ_CXX"
        echo "Tip (macOS): install LLVM and retry:"
        echo "  brew install llvm"
        echo "Or set explicit compiler env vars:"
        echo "  DINERO_FUZZ_CXX=/path/to/clang++ DINERO_FUZZ_CC=/path/to/clang ./run_fuzzing_suite.sh"
        exit 1
    fi

    echo "Using fuzz compiler:"
    echo "  CXX=$FUZZ_CXX"
    echo "  CC=$FUZZ_CC"
    if [ -n "$FUZZ_CMAKE_CXX_FLAGS" ]; then
        echo "  CXXFLAGS=$FUZZ_CMAKE_CXX_FLAGS"
    fi
    if [ -n "$FUZZ_CMAKE_EXE_LINKER_FLAGS" ]; then
        echo "  LDFLAGS=$FUZZ_CMAKE_EXE_LINKER_FLAGS"
    fi

    cxx_re="${FUZZ_CXX//\//\\/}"

    if [ ! -f "$cache_file" ] || \
       ! grep -q '^ENABLE_FUZZING:BOOL=ON$' "$cache_file" || \
       ! grep -q "^CMAKE_CXX_COMPILER:FILEPATH=${cxx_re}$" "$cache_file"; then
        needs_reconfigure=1
    fi

    if [ "$FUZZ_NEEDS_HOMEBREW_LIBCXX" -eq 1 ] && [ "$needs_reconfigure" -eq 0 ]; then
        if ! grep -Fq 'CMAKE_CXX_FLAGS:STRING=-stdlib=libc++' "$cache_file"; then
            needs_reconfigure=1
        fi
        if ! grep -Fq '/opt/homebrew/opt/llvm/lib/c++' "$cache_file"; then
            needs_reconfigure=1
        fi
    fi

    if [ "$needs_reconfigure" -eq 1 ]; then
        echo -e "${YELLOW}Configuring fuzz build...${NC}"
        cmake_args=(
            -S "$PROJECT_ROOT"
            -B "$BUILD_DIR"
            -DCMAKE_C_COMPILER="$FUZZ_CC"
            -DCMAKE_CXX_COMPILER="$FUZZ_CXX"
            -DCMAKE_BUILD_TYPE=Debug
            -DENABLE_TESTS=ON
            -DENABLE_FUZZING=ON
        )
        if [ -n "$FUZZ_CMAKE_CXX_FLAGS" ]; then
            cmake_args+=("-DCMAKE_CXX_FLAGS=${FUZZ_CMAKE_CXX_FLAGS}")
        fi
        if [ -n "$FUZZ_CMAKE_EXE_LINKER_FLAGS" ]; then
            cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=${FUZZ_CMAKE_EXE_LINKER_FLAGS}")
        fi
        cmake "${cmake_args[@]}"
    fi

    echo -e "${YELLOW}Building fuzz targets...${NC}"
    cmake --build "$BUILD_DIR" --parallel --target "${core_targets[@]}"

    # Optional extended targets are best-effort: stale harnesses should not
    # break the core consensus safety run.
    if [ "${DINERO_FUZZ_FULL:-0}" = "1" ]; then
        for t in \
            fuzz_script \
            fuzz_der \
            fuzz_sighash \
            fuzz_p2p_header \
            fuzz_varint \
            http_parser_fuzz \
            base64_fuzz \
            json_rpc_fuzz \
            fuzz_bulletproofs \
            fuzz_compact_block; do
            if cmake --build "$BUILD_DIR" --parallel --target "$t"; then
                :
            else
                echo -e "${YELLOW}Warning: optional fuzz target failed to build and will be skipped: $t${NC}"
            fi
        done
    fi
    echo ""
}

find_fuzzer_bin() {
    local fuzzer_name="$1"
    local candidate=""

    for candidate in \
        "$BUILD_DIR/fuzz/$fuzzer_name" \
        "$BUILD_DIR/tests/fuzz/$fuzzer_name" \
        "$BUILD_DIR/$fuzzer_name"; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

run_fuzzer() {
    local fuzzer_name="$1"
    local corpus_key="$2"
    local max_len="$3"
    local fuzzer_bin=""
    local corpus_dir=""
    local artifact_dir=""
    local before_crashes=0
    local after_crashes=0
    local new_crashes=0

    if ! fuzzer_bin="$(find_fuzzer_bin "$fuzzer_name")"; then
        echo -e "${YELLOW}⊘ SKIP${NC}: $fuzzer_name (not built)"
        TOTAL_SKIPPED=$((TOTAL_SKIPPED + 1))
        return 0
    fi

    TOTAL_RUNS=$((TOTAL_RUNS + 1))
    corpus_dir="$BUILD_DIR/fuzz_corpus/$corpus_key"
    artifact_dir="$BUILD_DIR/fuzz_crashes/$corpus_key"
    mkdir -p "$corpus_dir" "$artifact_dir"
    before_crashes=$(find "$artifact_dir" -maxdepth 1 -name 'crash-*' -type f 2>/dev/null | wc -l | tr -d ' ')

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Fuzzing: $fuzzer_name"
    echo "Binary:  $fuzzer_bin"
    echo "Corpus:  $corpus_dir"
    echo "Crashes: $artifact_dir"
    echo "Duration: ${DURATION}s"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    if "$fuzzer_bin" "$corpus_dir" \
        -artifact_prefix="$artifact_dir/" \
        -max_total_time="$DURATION" \
        -max_len="$max_len" \
        -print_final_stats=1 \
        -print_corpus_stats=1; then
        echo -e "${GREEN}✓ Fuzzing completed${NC}: $fuzzer_name"
    else
        echo -e "${RED}✗ Fuzzer exited with error${NC}: $fuzzer_name"
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
    fi

    after_crashes=$(find "$artifact_dir" -maxdepth 1 -name 'crash-*' -type f 2>/dev/null | wc -l | tr -d ' ')
    if [ "$after_crashes" -gt "$before_crashes" ]; then
        new_crashes=$((after_crashes - before_crashes))
        TOTAL_CRASH_FILES=$((TOTAL_CRASH_FILES + new_crashes))
        echo -e "${RED}⚠ New crashes found${NC}: $new_crashes"
        find "$artifact_dir" -maxdepth 1 -name 'crash-*' -type f -print | tail -n "$new_crashes"
    fi
}

ensure_fuzz_build

# Track results
TOTAL_RUNS=0
TOTAL_SKIPPED=0
TOTAL_FAILED=0
TOTAL_CRASH_FILES=0

# ============================================================================
# Run consensus safety fuzzers (matches fuzz/CMakeLists.txt D.3 targets)
# ============================================================================
echo "═══════════════════════════════════════════════════════════"
echo "  Running Consensus Safety Fuzzers"
echo "═══════════════════════════════════════════════════════════"

run_fuzzer "fuzz_premine" "premine" "1024"
run_fuzzer "fuzz_consensus_limits" "consensus_limits" "64"
run_fuzzer "fuzz_tx_deserialize" "tx" "100000"
run_fuzzer "fuzz_script_limits" "script_limits" "20000"

# Optional extended set
if [ "${DINERO_FUZZ_FULL:-0}" = "1" ]; then
    echo ""
    echo "Running extended fuzz set (DINERO_FUZZ_FULL=1)..."
    run_fuzzer "fuzz_script" "script" "10000"
    run_fuzzer "fuzz_der" "der" "200"
    run_fuzzer "fuzz_sighash" "sighash" "5000"
    run_fuzzer "fuzz_p2p_header" "p2p_header" "1024"
    run_fuzzer "fuzz_varint" "varint" "64"
    run_fuzzer "http_parser_fuzz" "http_parser" "2048"
    run_fuzzer "base64_fuzz" "base64" "512"
    run_fuzzer "json_rpc_fuzz" "json_rpc" "4096"
    run_fuzzer "fuzz_bulletproofs" "bulletproofs" "4096"
    run_fuzzer "fuzz_compact_block" "compact_block" "4096"
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Fuzzing Results Summary                                   ║"
echo "╠════════════════════════════════════════════════════════════╣"
printf "║  Fuzzers run: %-3d                                         ║\n" "$TOTAL_RUNS"
printf "║  Fuzzers skipped: %-3d                                     ║\n" "$TOTAL_SKIPPED"
printf "║  Fuzzer failures: %-3d                                     ║\n" "$TOTAL_FAILED"
printf "║  New crash files: %-3d                                     ║\n" "$TOTAL_CRASH_FILES"
echo "╠════════════════════════════════════════════════════════════╣"
echo "║  Corpus Statistics:                                        ║"

if [ -d "$BUILD_DIR/fuzz_corpus" ]; then
    for corpus in "$BUILD_DIR"/fuzz_corpus/*; do
        if [ -d "$corpus" ]; then
            count=$(find "$corpus" -type f | wc -l | tr -d ' ')
            printf "║    %-20s %5d inputs                     ║\n" "$(basename "$corpus"):" "$count"
        fi
    done
fi

echo "╚════════════════════════════════════════════════════════════╝"

if [ "$TOTAL_CRASH_FILES" -gt 0 ] || [ "$TOTAL_FAILED" -gt 0 ]; then
    echo ""
    echo -e "${RED}❌ Fuzzing reported failures/crashes.${NC}"
    echo "Review artifacts in: $BUILD_DIR/fuzz_crashes"
    echo "To reproduce: run the matching fuzzer binary with a crash file."
    exit 1
fi

echo ""
echo -e "${GREEN}✅ No new crashes found.${NC}"
echo "Fuzzing completed successfully."
