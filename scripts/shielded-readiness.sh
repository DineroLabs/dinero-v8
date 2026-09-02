#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${repo_dir}/build-shielded-readiness}"
evidence_dir="${SHIELDED_EVIDENCE_DIR:-${repo_dir}/artifacts/shielded-readiness}"
jobs="${SHIELDED_JOBS:-4}"
mkdir -p "${evidence_dir}"

cmake -S "${repo_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DDINERO_BUILD_QT=OFF -DENABLE_GRPC=OFF
cmake --build "${build_dir}" --parallel "${jobs}" --target \
  test_anchor_history test_shielded_circuit test_shielded_validation \
  test_shielded_cv_binding test_spartan_soundness test_shielded_serialization \
  test_shielded_serialization_oom test_shielded_generator_failclosed \
  test_shielded_empty_rangeproof_inflation test_shielded_epoch_reset \
  test_shielded_reindex_equivalence test_shielded_note_store_rollback \
  test_shielded_derivation test_shielded_prover_kit test_shielded_witness \
  test_shielded_output_feed

regex='^(AnchorHistory|ShieldedCircuit|ShieldedValidation|ShieldedCvBinding|SpartanSoundness|ShieldedSerialization|ShieldedSerializationOOM|ShieldedGeneratorFailClosed|ShieldedEmptyRangeProofInflation|ShieldedEpochReset|ShieldedReindexEquivalence|ShieldedNoteStoreRollback|ShieldedDerivation|ShieldedProverKit|ShieldedWitness|ShieldedOutputFeed)$'
ctest --test-dir "${build_dir}" --output-on-failure --no-tests=error \
  --parallel "${jobs}" -R "${regex}" --output-junit "${evidence_dir}/junit.xml" \
  | tee "${evidence_dir}/ctest.log"

readiness_status=0
python3 "${repo_dir}/tools/shielded_activation_readiness.py" \
  --source-only --repo "${repo_dir}" \
  --json "${evidence_dir}/activation-readiness.json" || readiness_status=$?

{
  git -C "${repo_dir}" rev-parse HEAD
  git -C "${repo_dir}" describe --tags --always --dirty
  cmake --version | head -1
  "${CXX:-c++}" --version | head -1
  uname -a
} > "${evidence_dir}/provenance.txt"

printf 'Shielded readiness evidence: %s\n' "${evidence_dir}"
if (( readiness_status != 0 )); then
  printf 'Activation preflight failed closed; see activation-readiness.json.\n' >&2
  exit "${readiness_status}"
fi
