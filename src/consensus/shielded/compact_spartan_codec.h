// Compact shielded v1 codec. The caller still verifies expanded proofs.
#pragma once

#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/r1cs.h"
#include <optional>
#include <span>
#include <vector>

namespace dinero::consensus::shielded {

// Layout comes from the verifier's circuit, never from untrusted proof dimensions.
// The version is the historical inner proof profile (0x04 output / 0x06 spend).
// Compact v1 retains the qualified DZE1 container tag and fixed circuit hashes.
// Accepted fields have canonical scalar/point encodings, but callers must still
// cryptographically verify the expanded proof. Historical decoding is untouched.
class CompactSpartanCodec {
  public:
    CompactSpartanCodec(uint8_t version, const zk::zkvm::R1CS &circuit);
    std::optional<std::vector<uint8_t>> Pack(std::span<const uint8_t> proof) const;
    std::optional<std::vector<uint8_t>> Expand(std::span<const uint8_t> compact) const;

  private:
    bool Check(std::span<const uint8_t> proof, bool compact) const;
    uint8_t version_;
    zk::zkvm::HyraxParams witness_, error_;
    size_t outer_rounds_, inner_rounds_, witness_rounds_, error_rounds_;
    size_t expanded_size_, error_offset_;
    std::vector<uint8_t> circuit_hash_;
};

} // namespace dinero::consensus::shielded
