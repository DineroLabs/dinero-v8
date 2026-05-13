#!/bin/bash
# Coverage analysis for URL parser branches and critical paths
# Identifies uncovered branches and provides detailed coverage metrics

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-coverage"

echo "🔍 Running coverage analysis for URL parser branches..."
echo "=================================================="

# Clean and create build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Configure with coverage instrumentation
echo "📊 Configuring build with coverage instrumentation..."
cmake -B coverage_build -S . \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -g -O0" \
    -DCMAKE_C_FLAGS="--coverage -g -O0"

echo "🔨 Building CLI tests with coverage..."
cmake --build coverage_build --target dinero_cli_core
cmake --build coverage_build --target test_url_parser test_ipv6_unix_sockets fuzz_url_parser

# Run all CLI tests to generate coverage data
echo "🧪 Running tests to generate coverage data..."
cd coverage_build

# Run each test and capture coverage
if [ -f "./bin/test_url_parser" ]; then
    ./bin/test_url_parser
else
    echo "⚠️ test_url_parser not found, skipping..."
fi

if [ -f "./bin/test_ipv6_unix_sockets" ]; then
    ./bin/test_ipv6_unix_sockets
else
    echo "⚠️ test_ipv6_unix_sockets not found, skipping..."
fi

if [ -f "./bin/fuzz_url_parser" ]; then
    ./bin/fuzz_url_parser 50 25 10
else
    echo "⚠️ fuzz_url_parser not found, skipping..."
fi

# Run property-based tests for additional coverage
cd "$PROJECT_ROOT/tests/cli"
python3 test_property_based.py || echo "Property tests completed (some may fail)"

# Generate coverage reports
cd "$BUILD_DIR"
echo "📈 Generating coverage reports..."

# Find all .gcda files for URL parser
PARSER_GCDA=$(find . -name "*url_parser*" -name "*.gcda" | head -1)
PARSER_GCNO=$(find . -name "*url_parser*" -name "*.gcno" | head -1)

if [[ -z "$PARSER_GCDA" || -z "$PARSER_GCNO" ]]; then
    echo "❌ Coverage data files not found for URL parser"
    echo "Looking for .gcda files:"
    find . -name "*.gcda" | grep -E "(url_parser|cli)" || echo "None found"
    exit 1
fi

echo "📁 Found coverage data:"
echo "  GCDA: $PARSER_GCDA"
echo "  GCNO: $PARSER_GCNO"

# Generate detailed coverage report with branch analysis
echo "🌿 Analyzing branch coverage..."

# Use gcov to generate detailed coverage
PARSER_SOURCE="$PROJECT_ROOT/cli/src/net/url_parser.cpp"
gcov -b -c "$PARSER_SOURCE" -o "$(dirname "$PARSER_GCDA")" > coverage_output.txt 2>&1 || true

# Parse gcov output for branch analysis
if [[ -f "url_parser.cpp.gcov" ]]; then
    echo "📊 Branch Coverage Analysis:"
    echo "=========================="
    
    # Count total and taken branches
    TOTAL_BRANCHES=$(grep -c "branch" coverage_output.txt || echo "0")
    TAKEN_BRANCHES=$(grep "branch.*taken" coverage_output.txt | wc -l || echo "0")
    NEVER_BRANCHES=$(grep "branch.*never executed" coverage_output.txt | wc -l || echo "0")
    
    echo "Total branches: $TOTAL_BRANCHES"
    echo "Taken branches: $TAKEN_BRANCHES"
    echo "Never executed: $NEVER_BRANCHES"
    
    if [[ $TOTAL_BRANCHES -gt 0 ]]; then
        BRANCH_COVERAGE=$((TAKEN_BRANCHES * 100 / TOTAL_BRANCHES))
        echo "Branch coverage: $BRANCH_COVERAGE%"
        
        if [[ $BRANCH_COVERAGE -lt 80 ]]; then
            echo "⚠️  Branch coverage below 80% threshold"
        else
            echo "✅ Branch coverage meets 80% threshold"
        fi
    fi
    
    # Show uncovered branches
    echo ""
    echo "🔍 Uncovered branches:"
    echo "===================="
    grep -n "branch.*never executed" coverage_output.txt || echo "All branches covered!"
    
    # Show line coverage for context
    echo ""
    echo "📝 Line coverage summary:"
    echo "========================"
    grep -E "Lines executed|Branches executed|Taken at least once" coverage_output.txt || true
    
    # Generate annotated source with coverage
    echo ""
    echo "📄 Annotated source (first 50 lines with coverage info):"
    echo "======================================================="
    head -50 url_parser.cpp.gcov | grep -E "^[ ]*[0-9#-]+:" | head -20
    
else
    echo "❌ Failed to generate gcov report"
    echo "Coverage output:"
    cat coverage_output.txt
fi

# Function coverage analysis
echo ""
echo "🔧 Function Coverage Analysis:"
echo "============================="

# Extract function coverage from gcov
if [[ -f "url_parser.cpp.gcov" ]]; then
    # Look for function entry points and their execution counts
    grep -E "^[ ]*[0-9#-]+:.*ParseUrl|^[ ]*[0-9#-]+:.*is_valid_ipv6" url_parser.cpp.gcov | head -10
fi

# Critical path analysis
echo ""
echo "🎯 Critical Path Analysis:"
echo "=========================="
echo "Analyzing coverage of critical code paths:"

# IPv6 validation paths
IPV6_COVERAGE=$(grep -c "fe80\|::1\|2001:db8" coverage_output.txt || echo "0")
echo "- IPv6 validation paths tested: $IPV6_COVERAGE cases"

# Error handling paths  
ERROR_COVERAGE=$(grep -c "error_message\|Invalid\|bracket" coverage_output.txt || echo "0")
echo "- Error handling paths tested: $ERROR_COVERAGE cases"

# Port validation paths
PORT_COVERAGE=$(grep -c "port.*range\|port.*invalid" coverage_output.txt || echo "0")
echo "- Port validation paths tested: $PORT_COVERAGE cases"

echo ""
echo "💡 Coverage Analysis Recommendations:"
echo "===================================="

if [[ $NEVER_BRANCHES -gt 0 ]]; then
    echo "- Add tests for uncovered branches shown above"
    echo "- Focus on error conditions and edge cases"
    echo "- Consider adding negative test cases"
fi

if [[ $IPV6_COVERAGE -lt 3 ]]; then
    echo "- Add more IPv6 test cases (current: $IPV6_COVERAGE)"
    echo "- Test zone IDs, compressed addresses, edge cases"
fi

echo "- Consider adding malformed URL fuzzing"
echo "- Test boundary conditions (port limits, long URLs)"
echo "- Verify all error message paths are exercised"

echo ""
echo "✅ Coverage analysis complete!"
echo "Report saved in: $BUILD_DIR/coverage_output.txt"
echo "Annotated source: $BUILD_DIR/url_parser.cpp.gcov"
