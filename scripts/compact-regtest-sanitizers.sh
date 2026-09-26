#!/usr/bin/env bash
# Full daemon/wallet lifecycle instrumentation, separate from codec-only fuzzing.
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_dir}"
build_dir="${COMPACT_SANITIZER_BUILD_DIR:-${repo_dir}/build-compact-sanitizers}"
evidence_dir="${COMPACT_SANITIZER_EVIDENCE_DIR:-${repo_dir}/artifacts/compact-sanitizers}"
jobs="${COMPACT_SANITIZER_JOBS:-2}"
mode="${1:-all}"
if [[ "${mode}" != all && "${mode}" != --test-only ]]; then
    printf 'usage: %s [--test-only]\n' "$0" >&2; exit 2
fi
# Refuse reused evidence: a stale green XML or report must not count as this run.
if [[ -e "${evidence_dir}" ]]; then
    printf 'Evidence directory must be new: %s\n' "${evidence_dir}" >&2; exit 2
fi
mkdir -p "${evidence_dir}/runtime"
evidence_dir="$(cd "${evidence_dir}" && pwd)"
python3 "${repo_dir}/scripts/ci/verify_compact_sanitizer_runtime.py" \
    "${evidence_dir}/runtime-probes" > "${evidence_dir}/runtime-probes.json"
flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
if [[ "${mode}" == all ]]; then
    cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="${flags}" -DCMAKE_CXX_FLAGS="${flags}" \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO='-O1 -g1 -DNDEBUG' \
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO='-O1 -g1 -DNDEBUG' \
        -DDINERO_BUILD_QT=OFF -DENABLE_GRPC=OFF -DENABLE_TESTS=ON -DENABLE_ZK=ON \
        -DDINERO_ENABLE_COMPACT_REGTEST=ON \
        -DDINERO_COMPACT_VECTOR_TEST_TIMEOUT=600 \
        2>&1 | tee "${evidence_dir}/configure.log"
fi
build_dir="$(cd "${build_dir}" && pwd)"
python3 "${repo_dir}/scripts/ci/check_shielded_sanitizer_coverage.py" \
    "${build_dir}" address,undefined --compact-daemon > "${evidence_dir}/coverage.json"
python3 "${repo_dir}/scripts/ci/test_check_compact_sanitizer_evidence.py" \
    > "${evidence_dir}/gate-self-tests.log" 2>&1
if [[ "${mode}" == all ]]; then
    cmake --build "${build_dir}" --parallel "${jobs}" --target dinerod \
        test_compact_regtest_vectors test_shielded_validation test_shielded_resource_limits \
        test_shielded_reindex_equivalence shielded_tx_builder test_packed_header_alignment \
        test_serialization_empty_buffers test_p2p_header_parser test_daemon_service_release \
        2>&1 | tee "${evidence_dir}/build.log"
fi
# Audit the linked daemon too; configured flags alone do not prove which binary ran.
python3 - "${build_dir}/dinerod" "${evidence_dir}" <<'PY'
import hashlib, json, platform, subprocess, sys
from pathlib import Path
binary, evidence = map(Path, sys.argv[1:])
symbols = subprocess.check_output(['nm', str(binary)], text=True)
required = ('__asan_init', '__ubsan_handle_')
if not all(symbol in symbols for symbol in required):
    raise SystemExit('daemon lacks required sanitizer runtime symbols')
receipt = {'binary': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
           'required_runtime_symbols': list(required), 'architecture': platform.machine(),
           'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
           'source_tree': subprocess.check_output(['git', 'rev-parse', 'HEAD^{tree}'], text=True).strip(),
           'source_changes': subprocess.check_output(['git', 'status', '--porcelain'], text=True),
           'scope': 'ASan/UBSan first-party daemon, wallet, consensus, Utreexo and fixed proofs; prebuilt OpenSSL and isolated RocksDB uninstrumented; no TSan claim'}
(evidence/'binary.json').write_text(json.dumps(receipt, indent=2)+'\n')
PY
leaks=1
if [[ "$(uname -s)" == Darwin ]]; then leaks=0; fi # Apple ASan does not provide LSan.
export ASAN_OPTIONS="detect_leaks=${leaks}:halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1:log_path=${evidence_dir}/runtime/asan"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=${evidence_dir}/runtime/ubsan"
export COMPACT_EVIDENCE_DIR="${evidence_dir}/compact"
printf 'ASAN_OPTIONS=%s\nUBSAN_OPTIONS=%s\n' "${ASAN_OPTIONS}" "${UBSAN_OPTIONS}" > "${evidence_dir}/runtime-options.txt"
# The preexisting integration tests include real wallet proving, signed child
# spend, full/CSN Utreexo state equality, activation reorg and reindex/restart.
set +e
ctest --test-dir "${build_dir}" --no-tests=error --output-on-failure -j 1 \
    -R '^(P2PHeaderParserAlignment|DaemonServiceRelease|RPCListenerStartup|PackedHeaderAlignment|SerializationEmptyBuffers|CompactRegtestFixedVectors|CompactRegtestVectorOracle|CompactProductionV6Vectors|CompactProductionV6Oracle|ShieldedResourceLimits|ShieldedReindexEquivalence|ShieldedAuthRelayLifecycle|CompactRegtestLifecycle|CsnManualInvalidation|CSNShieldedReorgInvertibility)$' \
    --output-junit "${evidence_dir}/ctest.xml" 2>&1 | tee "${evidence_dir}/ctest.log"
ctest_rc=${PIPESTATUS[0]}
printf '%s\n' "${ctest_rc}" > "${evidence_dir}/ctest.exit"
python3 "${repo_dir}/scripts/ci/check_compact_sanitizer_evidence.py" "${evidence_dir}" \
    > "${evidence_dir}/result.json"
gate_rc=$?
set -e
cat "${evidence_dir}/result.json"
if (( ctest_rc != 0 || gate_rc != 0 )); then exit 1; fi
