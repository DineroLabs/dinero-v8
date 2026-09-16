#!/usr/bin/env bash
# The pinned 8.1.13 baseline resolves proof metadata through its asynchronous
# wallet index. Only that baseline may wait for an absent wallet row after a
# reorg. Candidates use canonical coins and must verify on the first attempt.
verify_lifecycle_utreexo_proof() {
    local proofs="$1" verify attempt max_attempts=1
    if [[ "${LEGACY_PROOF_WALLET_WAIT:-0}" == 1 ]]; then max_attempts=101; fi
    for ((attempt=1; attempt<=max_attempts; ++attempt)); do
        verify="$(rpc_result blockchain.verifyutxoproofs_batch "$(jq -c '[.result.proofs]' <<<"${proofs}")")"
        if jq -e --arg root "$(jq -r '.result.utreexo_root' <<<"${proofs}")" \
            '.result.valid == 1 and .result.invalid == 0 and .result.utreexo_root == $root' \
            <<<"${verify}" >/dev/null; then
            if (( attempt > 1 )); then info "legacy wallet proof metadata ready after ${attempt} attempts"; fi
            return 0
        fi
        # Never retry bad proofs, changed roots, malformed replies, transport
        # errors or candidate failures. Keep the original proof fixed throughout.
        if (( attempt < max_attempts )) && \
            jq -e --arg root "$(jq -r '.result.utreexo_root' <<<"${proofs}")" \
            '.result.valid == 0 and .result.invalid == 1 and .result.utreexo_root == $root and
             (.result.results | length) == 1 and
             .result.results[0].error_code == "utxo-not-found"' <<<"${verify}" >/dev/null; then
            sleep 0.1
        else
            fail "unshield output proof verification failed: ${verify}"
        fi
    done
}
