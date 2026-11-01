#!/usr/bin/env bash
# Run the consensus-validation gates (CV1-CV4) on one configured build.
#   run_gates.sh <build_dir> <plat_tag> <off|on>
# Baselines: baselines/<gate>_<plat_tag>_<cfg>.json. A missing baseline is recorded into
# $CV_CANDIDATE_DIR (default: <build_dir>/cv-candidates) for review and reported
# "MISSING_REFERENCE" (rc 2); it is never checked against in the same run.
# Prints "GATE <id> PASS|FAIL rc=N|MISSING_REFERENCE" per gate; exits with the worst rc.
set -uo pipefail
build=$1 plat=$2 cfg=$3
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../../.." && pwd)
cand=${CV_CANDIDATE_DIR:-$build/cv-candidates}; mkdir -p "$cand"
# CV3 needs the CMake File API codemodel; request it and re-run configure (no build).
mkdir -p "$build/.cmake/api/v1/query" && touch "$build/.cmake/api/v1/query/codemodel-v2"
cmake "$build" >"$build/cv-reconfigure.log" 2>&1 || { echo "GATE ALL FAIL rc=2 (cmake re-configure failed)"; exit 2; }
worst=0
gate() { # id baseline-name cmd...
  local id=$1 name=$2; shift 2
  local ref="$here/baselines/${name}_${plat}_${cfg}.json" rc
  if [ ! -f "$ref" ]; then
    "$@" record "$cand/${name}_${plat}_${cfg}.json" >/dev/null; rc=$?
    echo "GATE $id MISSING_REFERENCE (candidate $cand/${name}_${plat}_${cfg}.json, record rc=$rc)"
    rc=2
  else
    "$@" check "$ref"; rc=$?
    if [ $rc -eq 0 ]; then echo "GATE $id PASS"; else echo "GATE $id FAIL rc=$rc"; fi
  fi
  [ $rc -gt $worst ] && worst=$rc
  return 0
}
cv1() { python3 "$here/header_reach.py" "$1" "$build" "$root" "$here/cv1_rules.json" "$2"; }
cv2() { python3 "$here/rule_sites.py" "$root" "$here/rule_sites.tsv"; }
cv3() { python3 "$here/live_callers.py" "$1" "$build" "$root" "$here/cv3_rules.json" "$2"; }
cv4() { python3 "$here/dup_objects.py" "$1" "$build" "$root" "$2"; }
gate CV1 cv1_header_reach cv1
cv2; rc=$?; if [ $rc -eq 0 ]; then echo "GATE CV2 PASS"; else echo "GATE CV2 FAIL rc=$rc"; fi; [ $rc -gt $worst ] && worst=$rc
gate CV3 cv3_live_callers cv3
gate CV4 cv4_dup_objects cv4
exit $worst
