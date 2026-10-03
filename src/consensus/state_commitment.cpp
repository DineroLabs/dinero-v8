#include "consensus/state_commitment.h"

#include <cstring>
#include <stdexcept>

namespace dinero::consensus {

namespace {

/// True when the script carries the DNRS tag at all — well-formed or not.
/// Deliberately loose: a truncated or wrong-version commitment must still be
/// SEEN, so it can be reported as Malformed rather than silently as Missing.
/// Treating a corrupt commitment as absent is how a truncation slips past a
/// presence check.
bool CarriesTag(const std::vector<uint8_t>& s) {
    if (s.size() < StateCommitment::OFFSET_VERSION) return false;
    if (s[StateCommitment::OFFSET_OPRETURN] != 0x6a) return false;
    return std::memcmp(s.data() + StateCommitment::OFFSET_MAGIC,
                       StateCommitment::MAGIC_BYTES, 4) == 0;
}

}  // namespace

std::vector<uint8_t> BuildStateCommitmentScript(const uint256& root) {
    return BuildStateCommitmentScript(root, StateCommitmentEncoding::Legacy);
}

std::vector<uint8_t> BuildStateCommitmentScript(const uint256& root, StateCommitmentEncoding encoding) {
    if (encoding != StateCommitmentEncoding::Legacy && encoding != StateCommitmentEncoding::Orchard)
        throw std::invalid_argument("unsupported state commitment encoding");
    std::vector<uint8_t> script;
    script.reserve(StateCommitment::SCRIPT_SIZE);
    script.push_back(0x6a);                                        // OP_RETURN
    script.push_back(static_cast<uint8_t>(StateCommitment::PAYLOAD_SIZE));
    script.insert(script.end(), StateCommitment::MAGIC_BYTES,
                  StateCommitment::MAGIC_BYTES + 4);
    script.push_back(static_cast<uint8_t>(encoding));
    script.insert(script.end(), root.data, root.data + 32);
    return script;
}

std::optional<uint256> ParseStateCommitmentScript(const std::vector<uint8_t>& script) {
    return ParseStateCommitmentScript(script, StateCommitmentEncoding::Legacy);
}

std::optional<uint256> ParseStateCommitmentScript(const std::vector<uint8_t>& script, StateCommitmentEncoding encoding) {
    if (encoding != StateCommitmentEncoding::Legacy && encoding != StateCommitmentEncoding::Orchard)
        return std::nullopt;
    // Exact size. A longer script that merely starts with the right bytes is
    // NOT this commitment: accepting a prefix would let trailing bytes ride
    // along unauthenticated, and would make the encoding non-canonical.
    if (script.size() != StateCommitment::SCRIPT_SIZE) return std::nullopt;
    if (script[StateCommitment::OFFSET_OPRETURN] != 0x6a) return std::nullopt;
    if (script[StateCommitment::OFFSET_PUSHLEN] != StateCommitment::PAYLOAD_SIZE) {
        return std::nullopt;
    }
    if (std::memcmp(script.data() + StateCommitment::OFFSET_MAGIC,
                    StateCommitment::MAGIC_BYTES, 4) != 0) {
        return std::nullopt;
    }
    if (script[StateCommitment::OFFSET_VERSION] != static_cast<uint8_t>(encoding)) {
        return std::nullopt;
    }
    uint256 root;
    std::memcpy(root.data, script.data() + StateCommitment::OFFSET_ROOT, 32);
    return root;
}

std::vector<size_t> FindStateCommitmentCandidates(const Transaction& coinbase) {
    std::vector<size_t> out;
    for (size_t i = 0; i < coinbase.vout.size(); ++i) {
        if (CarriesTag(coinbase.vout[i].scriptPubKey)) out.push_back(i);
    }
    return out;
}

StateCommitmentLookup FindStateCommitment(const Transaction& coinbase) {
    return FindStateCommitment(coinbase, StateCommitmentEncoding::Legacy);
}

StateCommitmentLookup FindStateCommitment(const Transaction& coinbase, StateCommitmentEncoding encoding) {
    StateCommitmentLookup r;
    if ((encoding != StateCommitmentEncoding::Legacy && encoding != StateCommitmentEncoding::Orchard) ||
        (encoding == StateCommitmentEncoding::Orchard && !coinbase.IsCoinbase())) {
        r.status = StateCommitmentStatus::Malformed;
        return r;
    }

    const auto candidates = FindStateCommitmentCandidates(coinbase);
    if (candidates.empty()) {
        r.status = StateCommitmentStatus::Missing;
        return r;
    }
    // Exactly one. The DNRF precedent scans backwards and lets the last match
    // win, which silently tolerates duplicates; a second commitment would let
    // a block claim two different shielded states, so it is rejected outright.
    if (candidates.size() > 1) {
        r.status = StateCommitmentStatus::Duplicate;
        return r;
    }

    const auto& output = coinbase.vout[candidates[0]];
    const auto parsed = ParseStateCommitmentScript(output.scriptPubKey, encoding);
    if (!parsed.has_value() || (encoding == StateCommitmentEncoding::Orchard &&
        (output.value.GetUna() != 0 || output.is_confidential || !output.commitment.empty() ||
         !output.range_proof.empty() || !output.nonce.empty()))) {
        r.status = StateCommitmentStatus::Malformed;
        return r;
    }

    r.status = StateCommitmentStatus::Ok;
    r.index = candidates[0];
    r.root = *parsed;
    return r;
}

}  // namespace dinero::consensus
