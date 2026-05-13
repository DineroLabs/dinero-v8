#!/bin/bash
set -euo pipefail

# Coverage gate for URL parser - ensures real file is tested
echo "🔍 Running coverage analysis for URL parser..."

# Auto-detect build system
GEN="-G Ninja"
if ! command -v ninja >/dev/null 2>&1; then
  echo "⚠️ Ninja not found; using Unix Makefiles"
  GEN="-G Unix Makefiles"
fi

# Build with coverage instrumentation
cmake -S . -B build-cov $GEN \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDIN_BUILD_CLI=ON -DDIN_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping"

cmake --build build-cov -j4 --target test_url_parser

# Run tests with coverage
LLVM_PROFILE_FILE=cli.profraw ./build-cov/tests/cli/test_url_parser

# Generate coverage report
llvm-profdata merge -sparse cli.profraw -o cli.profdata

# Check coverage for the real parser file
COVERAGE=$(llvm-cov report build-cov/tests/cli/test_url_parser -instr-profile=cli.profdata \
  | grep 'cli/src/net/url_parser.cpp' | awk '{print $NF}' | sed 's/%//')

if [ -z "$COVERAGE" ]; then
    echo "❌ No coverage found for cli/src/net/url_parser.cpp - tests may not be using real parser"
    exit 1
fi

THRESHOLD=80
if (( $(echo "$COVERAGE < $THRESHOLD" | bc -l) )); then
    echo "❌ Coverage too low: ${COVERAGE}% (threshold: ${THRESHOLD}%)"
    exit 1
fi

echo "✅ Coverage gate passed: ${COVERAGE}% (threshold: ${THRESHOLD}%)"
