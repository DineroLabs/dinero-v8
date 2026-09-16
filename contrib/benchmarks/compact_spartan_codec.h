// Research codec: tests/fuzzers and explicitly enabled compact-regtest builds only.
#pragma once

#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/r1cs.h"
#include <optional>
#include <span>
#include <vector>

namespace dinero::experimental {

// Layout comes from the verifier's circuit, never from untrusted proof dimensions.
// The version is the historical inner proof profile (0x04 output / 0x06 spend).
// DZE1 is an experimental file tag, not a proposed/assigned consensus version.
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

} // namespace dinero::experimental
