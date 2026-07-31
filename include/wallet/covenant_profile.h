#pragma once

#include "consensus/covenants.h"
#include "primitives/transaction.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::wallet::covenant {

inline constexpr uint8_t PROFILE_VERSION = 1;
inline constexpr uint8_t TAPSCRIPT_LEAF_VERSION = 0xc0;

enum class ProfileType : uint8_t {
    CTV = 1,
    CCV = 2,
};

struct Output {
    AmountUna value{AmountUna::Zero()};
    std::vector<uint8_t> scriptPubKey;
};

struct Input {
    TxOutPoint prevout;
    uint32_t sequence{0xfffffffeU};
};

struct TaprootArtifact {
    std::array<uint8_t, 32> internalKey{};
    std::array<uint8_t, 32> merkleRoot{};
    uint8_t outputKeyParity{0};
    std::vector<uint8_t> tapscript;
    std::vector<uint8_t> controlBlock;
    std::vector<uint8_t> scriptPubKey;
};

/**
 * A complete, deterministic CTV construction artifact.
 *
 * templateTx contains zero prevouts. CTV does not commit to prevout
 * identifiers, so BuildCTVSpend replaces only those identifiers while
 * retaining every committed field and installs the script-path witness.
 */
struct CTVPlan {
    Transaction templateTx;
    uint32_t covenantInputIndex{0};
    std::array<uint8_t, 32> templateHash{};
    TaprootArtifact taproot;
    std::string recoveryDescriptor;
    std::string descriptorId;
};

/**
 * Construct a profile-v1 CTV plan.
 *
 * inputSequences fixes both the number of inputs and every committed
 * sequence. At least one input and one transparent output are required.
 */
CTVPlan BuildCTVPlan(
    const std::vector<uint32_t>& inputSequences,
    uint32_t covenantInputIndex,
    const std::vector<Output>& outputs,
    uint32_t lockTime = 0,
    int32_t version = 2);

/**
 * Recover and fully re-derive a CTV plan from a checksummed descriptor.
 * Throws std::invalid_argument for malformed, non-canonical, or inconsistent
 * descriptors.
 */
CTVPlan RecoverCTVPlan(const std::string& descriptor);

/**
 * Build the witness-complete transaction committed by a CTV plan.
 *
 * inputs must exactly match the descriptor's input count and sequences.
 * Only prevout identifiers are supplied at spend time.
 */
Transaction BuildCTVSpend(
    const CTVPlan& plan,
    const std::vector<TxOutPoint>& prevouts);

struct CCVPlan {
    consensus::ContractState state;
    TaprootArtifact taproot;
    std::string recoveryDescriptor;
    std::string descriptorId;
};

/**
 * Construct a profile-v1 permissionless CCV state output. The v1 contract
 * code is the successor-binding tapscript:
 *
 *   OP_CHECKCONTRACTVERIFY OP_TRUE
 *
 * This script authenticates continuity, not an owner. Any party able to spend
 * an additional fee input can choose nextData and advance the state. It is a
 * regtest construction/recovery profile, not an owner-authorized contract.
 */
CCVPlan BuildCCVPlan(
    uint32_t counter,
    const std::vector<uint8_t>& data);

/**
 * Recover and fully re-derive a CCV state output from a checksummed
 * descriptor.
 */
CCVPlan RecoverCCVPlan(const std::string& descriptor);

struct CCVTransition {
    Transaction tx;
    CCVPlan successor;
};

/**
 * Build a witness-complete CCV transition.
 *
 * The covenant input and successor output are fixed at index zero. Additional
 * inputs and outputs may fund fees, but the CCV value is preserved exactly.
 * Additional inputs are left unsigned for the normal wallet signer.
 * No signature authorizes the covenant input itself.
 */
CCVTransition BuildCCVTransition(
    const CCVPlan& current,
    const std::vector<Input>& inputs,
    AmountUna covenantValue,
    const std::vector<uint8_t>& nextData,
    const std::vector<Output>& additionalOutputs = {},
    uint32_t lockTime = 0,
    int32_t version = 2);

/**
 * Decode enough of a descriptor to route it safely without trusting caller
 * metadata. The checksum and profile version are verified.
 */
ProfileType DescriptorType(const std::string& descriptor);

} // namespace dinero::wallet::covenant
