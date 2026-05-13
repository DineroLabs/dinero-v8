#!/bin/bash
# Phase M.0: Comprehensive .GetHex() Violation Audit
# Categorizes violations by severity and location

echo "🔍 Phase M.0: .GetHex() Violation Audit"
echo "========================================"
echo ""

# Define critical paths (consensus + daemon)
CONSENSUS_PATHS="src/consensus/ include/consensus/"
DAEMON_PATHS="src/daemon/ include/daemon/"

echo "📊 CRITICAL VIOLATIONS (Consensus Layer)"
echo "----------------------------------------"
echo ""

echo "1️⃣ String Comparisons:"
grep -rn "\.GetHex()" $CONSENSUS_PATHS 2>/dev/null | \
  grep -v "// Phase M.0" | \
  grep -E "(==|!=).*\.GetHex\(\)|\.GetHex\(\).*(==|!=)" | \
  grep -v "\.substr" | \
  cat -n

echo ""
echo "2️⃣ Early Downgrade (string variable assignment):"
grep -rn "std::string.*=.*\.GetHex\(\)" $CONSENSUS_PATHS 2>/dev/null | \
  grep -v "// Phase M.0" | \
  grep -v "GetHex().substr" | \
  grep -v "logger" | \
  grep -v "MPLOG" | \
  grep -v "g_logger" | \
  grep -v "std::cout" | \
  grep -v "std::cerr" | \
  cat -n

echo ""
echo "3️⃣ Non-logging inline uses in consensus:"
grep -rn "\.GetHex\(\)" $CONSENSUS_PATHS 2>/dev/null | \
  grep -v "// Phase M.0" | \
  grep -v "logger" | \
  grep -v "MPLOG" | \
  grep -v "g_logger" | \
  grep -v "std::cout" | \
  grep -v "std::cerr" | \
  grep -v "GetHex().substr" | \
  grep -v "Get.*Hex()" | \
  head -20 | \
  cat -n

echo ""
echo ""
echo "⚠️  MEDIUM VIOLATIONS (Daemon Layer)"
echo "-------------------------------------"
echo ""

echo "1️⃣ String Comparisons in daemon:"
grep -rn "\.GetHex()" $DAEMON_PATHS 2>/dev/null | \
  grep -v "// Phase M.0" | \
  grep -E "(==|!=).*\.GetHex\(\)|\.GetHex\(\).*(==|!=)" | \
  grep -v "\.substr" | \
  cat -n

echo ""
echo "2️⃣ Early Downgrade in daemon:"
grep -rn "std::string.*=.*\.GetHex\(\)" $DAEMON_PATHS 2>/dev/null | \
  grep -v "// Phase M.0" | \
  grep -v "GetHex().substr" | \
  grep -v "logger" | \
  grep -v "MPLOG" | \
  grep -v "std::cout" | \
  grep -v "std::cerr" | \
  cat -n

echo ""
echo ""
echo "✅ ACCEPTABLE USES"
echo "------------------"
echo ""

echo "1️⃣ Logging (inline .GetHex() in log statements):"
grep -rn "\.GetHex\(\)" src/ include/ 2>/dev/null | \
  grep -E "logger|MPLOG|g_logger|std::cout|std::cerr" | \
  grep -v ".md:" | \
  wc -l

echo ""
echo "2️⃣ RPC Boundaries (methods in src/rpc/):"
grep -rn "\.GetHex\(\)" src/rpc/ 2>/dev/null | \
  grep -v "// Phase M.0" | \
  wc -l

echo ""
echo "3️⃣ Storage Layer (chain_db.cpp, block_storage.cpp):"
grep -rn "\.GetHex\(\)" src/storage/ 2>/dev/null | \
  wc -l

echo ""
echo "4️⃣ Helper methods (GetTxIdHex, Get*Hex accessors):"
grep -rn "Get.*Hex.*const.*{.*return.*\.GetHex\(\)" include/ src/ 2>/dev/null | \
  grep -v ".md:" | \
  wc -l

echo ""
echo ""
echo "📈 SUMMARY STATISTICS"
echo "====================="
echo ""

TOTAL=$(grep -r "\.GetHex()" src/ include/ 2>/dev/null | grep -v ".md:" | wc -l | tr -d ' ')
CRITICAL_CONSENSUS=$(grep -rn "\.GetHex()" $CONSENSUS_PATHS 2>/dev/null | grep -v "// Phase M.0" | grep -v "logger" | grep -v "g_logger" | grep -v "std::cout" | grep -v "std::cerr" | grep -v "GetHex().substr" | grep -v "Get.*Hex()" | wc -l | tr -d ' ')
MEDIUM_DAEMON=$(grep -rn "\.GetHex()" $DAEMON_PATHS 2>/dev/null | grep -v "// Phase M.0" | grep -v "logger" | grep -v "MPLOG" | grep -v "std::cout" | grep -v "std::cerr" | grep -v "GetHex().substr" | wc -l | tr -d ' ')
LOGGING=$(grep -rn "\.GetHex\(\)" src/ include/ 2>/dev/null | grep -E "logger|MPLOG|g_logger|std::cout|std::cerr" | grep -v ".md:" | wc -l | tr -d ' ')
RPC=$(grep -rn "\.GetHex\(\)" src/rpc/ 2>/dev/null | wc -l | tr -d ' ')
STORAGE=$(grep -rn "\.GetHex\(\)" src/storage/ 2>/dev/null | wc -l | tr -d ' ')

echo "Total .GetHex() calls in source:     $TOTAL"
echo ""
echo "🔴 Critical (consensus):              $CRITICAL_CONSENSUS"
echo "🟡 Medium (daemon):                   $MEDIUM_DAEMON"
echo ""
echo "✅ Logging:                           $LOGGING"
echo "✅ RPC boundaries:                    $RPC"
echo "✅ Storage layer:                     $STORAGE"
echo ""
echo "Target: 🎯 0 critical, 0 medium violations"
echo ""
