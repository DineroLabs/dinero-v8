#!/bin/bash
set -euo pipefail

# Coverage budget enforcement for key CLI files
echo "📊 Checking coverage budget for key files..."

# Key files and their minimum coverage thresholds
declare -A COVERAGE_BUDGET=(
    ["cli/src/net/url_parser.cpp"]=80
    ["cli/src/commands/doctor.cpp"]=70
    ["src/cli/rpc_api.cpp"]=70
    ["src/cli/main.cpp"]=60
    ["rpc/rpc_server.cpp"]=65
    ["rpc/auth_cookie.cpp"]=75
    ["rpc/json_rpc_parser.cpp"]=70
)

# Build with coverage if not already built
if [ ! -d "build-cov" ]; then
    echo "Building with coverage instrumentation..."
    GEN="-G Ninja"
    if ! command -v ninja >/dev/null 2>&1; then
        GEN="-G Unix Makefiles"
    fi
    
    cmake -S . -B build-cov $GEN \
        -DCMAKE_BUILD_TYPE=Debug \
        -DDIN_BUILD_CLI=ON -DDIN_BUILD_TESTS=ON \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
        -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
    
    cmake --build build-cov -j4 --target test_url_parser test_mutation test_doctor
fi

# Run tests with coverage
LLVM_PROFILE_FILE=cli.profraw ./build-cov/bin/test_url_parser
LLVM_PROFILE_FILE=cli.profraw ./build-cov/bin/test_mutation
LLVM_PROFILE_FILE=cli.profraw ./build-cov/bin/test_doctor

# Generate coverage report
llvm-profdata merge -sparse cli.profraw -o cli.profdata

FAILED_FILES=()

# Check each file against its budget
for file in "${!COVERAGE_BUDGET[@]}"; do
    threshold=${COVERAGE_BUDGET[$file]}
    
    if [ ! -f "$file" ]; then
        echo "⚠️  File not found: $file (skipping)"
        continue
    fi
    
    # Extract coverage percentage for this specific file
    COVERAGE=$(llvm-cov report build-cov/bin/test_url_parser -instr-profile=cli.profdata \
        | grep "$file" | awk '{print $NF}' | sed 's/%//' || echo "0")
    
    if [ -z "$COVERAGE" ] || [ "$COVERAGE" = "0" ]; then
        echo "❌ $file: No coverage data (0%)"
        FAILED_FILES+=("$file")
        continue
    fi
    
    if (( $(echo "$COVERAGE < $threshold" | bc -l) )); then
        echo "❌ $file: ${COVERAGE}% < ${threshold}% (FAILED)"
        FAILED_FILES+=("$file")
    else
        echo "✅ $file: ${COVERAGE}% >= ${threshold}% (PASSED)"
    fi
done

# Summary
if [ ${#FAILED_FILES[@]} -eq 0 ]; then
    echo "🎉 All files meet coverage budget requirements"
    exit 0
else
    echo "💥 ${#FAILED_FILES[@]} files failed coverage budget:"
    for file in "${FAILED_FILES[@]}"; do
        echo "  - $file"
    done
    exit 1
fi
