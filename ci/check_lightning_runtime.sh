#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.6: Enforcement Layer 3 - Runtime Determinism Guard
# ═══════════════════════════════════════════════════════════════════════════
# Ensures Lightning runtime is deterministic (Phase 8.5 invariants)
#
# Forbidden patterns in source code:
# - #include <thread>              (background threads forbidden)
# - #include <chrono>              (wall clock time forbidden)
# - std::thread                    (threading forbidden)
# - std::chrono::system_clock      (wall time forbidden)
# - std::chrono::steady_clock      (monotonic time forbidden)
# - std::this_thread::sleep        (polling forbidden)
#
# Exception: <chrono> is allowed ONLY for duration types (e.g., std::chrono::milliseconds)
#            as long as system_clock/steady_clock are NOT used
#
# Exit code:
# - 0: No violations detected
# - 1: Determinism violation detected (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 8.6 - Enforcement Layer 3: Runtime Determinism Guard"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

VIOLATIONS=0

# Phase 8.5: ONLY check lightningd daemon (NOT library)
# Rationale:
#   - lightningd = Pure event-driven state machine (MUST be deterministic)
#   - src/lightning/ = Library code (can have threads until Phase 10+ networking redesign)
#   - include/lightning/ = Library headers (can have threads until Phase 10+ networking redesign)
#
# Architectural guarantee:
#   - lightningd does NOT link dinero_lightning library
#   - lightningd does NOT include lightning_peer.h
#   - lightningd is already isolated (see CMakeLists.txt)
#
# Phase 10+ will redesign src/lightning/ as event-driven
LIGHTNING_DIRS=(
  "src/lightningd"
)

echo "[1/3] Checking for forbidden threading patterns..."
echo ""

# Pattern 1: #include <thread>
echo "  Checking: #include <thread>"
for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
  if [ ! -d "$lightning_dir" ]; then
    continue
  fi

  if grep -r "#include <thread>" "$lightning_dir" 2>/dev/null; then
    echo ""
    echo "    ❌ VIOLATION: std::thread usage forbidden (Phase 8.5)"
    echo "    Location: $lightning_dir"
    echo "    Reason: Lightning MUST be purely event-driven (no background threads)"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

# Pattern 2: std::thread usage (even without include - catches copy-paste)
echo "  Checking: std::thread usage"
for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
  if [ ! -d "$lightning_dir" ]; then
    continue
  fi

  # Exclude comments (// and /* */ style)
  if grep -r "std::thread" "$lightning_dir" 2>/dev/null | grep -v "//\|^\s*\*\| \* "; then
    echo ""
    echo "    ❌ VIOLATION: std::thread usage detected (Phase 8.5)"
    echo "    Location: $lightning_dir"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

echo ""
echo "[2/3] Checking for forbidden time sources..."
echo ""

# Pattern 3: system_clock (wall time forbidden)
echo "  Checking: std::chrono::system_clock"
for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
  if [ ! -d "$lightning_dir" ]; then
    continue
  fi

  if grep -r "system_clock" "$lightning_dir" 2>/dev/null | grep -v "//\|^\s*\*\| \* "; then
    echo ""
    echo "    ❌ VIOLATION: Wall clock time forbidden (Phase 8.5)"
    echo "    Location: $lightning_dir"
    echo "    Reason: Lightning time source MUST be block height only"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

# Pattern 4: steady_clock (monotonic time forbidden)
echo "  Checking: std::chrono::steady_clock"
for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
  if [ ! -d "$lightning_dir" ]; then
    continue
  fi

  if grep -r "steady_clock" "$lightning_dir" 2>/dev/null | grep -v "//\|^\s*\*\| \* "; then
    echo ""
    echo "    ❌ VIOLATION: Monotonic clock forbidden (Phase 8.5)"
    echo "    Location: $lightning_dir"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

# Pattern 5: sleep patterns (polling forbidden)
echo "  Checking: std::this_thread::sleep"
for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
  if [ ! -d "$lightning_dir" ]; then
    continue
  fi

  if grep -r "sleep_for\|sleep_until" "$lightning_dir" 2>/dev/null | grep -v "//\|^\s*\*\| \* "; then
    echo ""
    echo "    ❌ VIOLATION: sleep_for/sleep_until forbidden (Phase 8.5)"
    echo "    Location: $lightning_dir"
    echo "    Reason: Lightning MUST NOT poll - purely event-driven"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

echo ""
echo "[3/3] Checking for implicit time dependencies..."
echo ""

# Pattern 6: time(), gettimeofday(), clock_gettime() (C-style time)
CTIME_PATTERNS=(
  "time("
  "gettimeofday"
  "clock_gettime"
)

for pattern in "${CTIME_PATTERNS[@]}"; do
  echo "  Checking: $pattern"
  for lightning_dir in "${LIGHTNING_DIRS[@]}"; do
    if [ ! -d "$lightning_dir" ]; then
      continue
    fi

    if grep -r "$pattern" "$lightning_dir" 2>/dev/null | grep -v "//\|^\s*\*\| \* "; then
      echo ""
      echo "    ❌ VIOLATION: C-style time function forbidden: $pattern"
      echo "    Location: $lightning_dir"
      echo ""
      VIOLATIONS=$((VIOLATIONS + 1))
    fi
  done
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
  echo "✅ PASS: Lightning runtime determinism enforced"
  echo "   Phase 8.5 invariants verified:"
  echo "   ✅ NO threads"
  echo "   ✅ NO wall clocks"
  echo "   ✅ NO sleep/polling"
  echo "   ✅ Purely event-driven"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 0
else
  echo "❌ FAIL: $VIOLATIONS violation(s) detected"
  echo "   Lightning MUST be deterministic (Phase 8.5)"
  echo "   Time source: Block height ONLY"
  echo "   Runtime model: Purely event-driven (no threads/timers)"
  echo "   Phase 8.6 Enforcement Layer 3: FAILED"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 1
fi
