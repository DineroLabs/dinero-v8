#!/usr/bin/env bash
# Run the consensus-validation gate CV1 on one configured build.
#   run_gates.sh <build_dir> <plat_tag> <off|on>
# Baselines: baselines/<gate>_<plat_tag>_<cfg>.json. A missing baseline is recorded into
# $CV_CANDIDATE_DIR (default: <build_dir>/cv-candidates) for review and reported
# "MISSING_REFERENCE" (rc 2); it is never checked against in the same run.
# Prints "GATE <id> PASS|FAIL rc=N|MISSING_REFERENCE" per gate; exits with the worst rc.
set -uo pipefail
build=$1 plat=$2 cfg=$3
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../../.." && pwd)
cand=${CV_CANDIDATE_DIR:-$build/cv-candidates}; mkdir -p "$cand"
worst=0
gate() { # id baseline-name cmd...
  local id=$1 name=$2; shift 2
  local ref="$here/baselines/${name}_${plat}_${cfg}.json" rc
  if [ ! -f "$ref" ]; then
    "$@" record "$cand/${name}_${plat}_${cfg}.json"; rc=$?
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
gate CV1 cv1_header_reach cv1
exit $worst
