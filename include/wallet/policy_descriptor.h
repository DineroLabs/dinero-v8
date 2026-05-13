#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

/**
 * Policy template types for Taproot outputs.
 * V1 constraint: max 3 leaves, max depth 2.
 */
enum class PolicyTemplate : uint8_t {
    STANDARD  = 0,  // Key-path only, no script tree
    PROTECTED = 1,  // Key-path(user) + panic leaf + recovery leaf
    ESCROW    = 2   // Key-path(buyer) + attestor leaf + timeout leaf
};

/**
 * Policy Descriptor — identifies the spending policy for a Taproot output.
 *
 * policy_id = TaggedHash("dinero/policy/v1", template_type || template_version || params)
 *
 * The policy_id is deterministic: same params always produce the same ID.
 * This allows re-deriving spending paths from seed + policy params alone.
 */
struct PolicyDescriptor {
    PolicyTemplate template_type;
    uint16_t template_version = 1;
    std::vector<uint8_t> params;  // Template-specific serialized params

    /**
     * Compute deterministic policy_id.
     * policy_id = TaggedHash("dinero/policy/v1", type(1) || version(2 LE) || params(N))
     */
    std::array<uint8_t, 32> ComputePolicyId() const;

    /**
     * Serialize to canonical byte representation.
     * Format: type(1) || version(2 LE) || params_len(2 LE) || params(N)
     */
    std::vector<uint8_t> Serialize() const;

    /**
     * Deserialize from bytes.
     * @throws std::runtime_error on malformed data
     */
    static PolicyDescriptor Deserialize(const std::vector<uint8_t>& data);

    /**
     * Human-readable template name.
     */
    static std::string TemplateName(PolicyTemplate t);
};

} // namespace dinero
