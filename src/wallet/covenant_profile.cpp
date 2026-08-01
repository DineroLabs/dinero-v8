#include "wallet/covenant_profile.h"

#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "crypto/tagged_hash.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/key_origin.h"
#include "wallet/taproot_tx_signer.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace dinero::wallet::covenant {
namespace {

constexpr char DESCRIPTOR_PREFIX[] = "dncov1:";
constexpr std::array<uint8_t, 6> DESCRIPTOR_MAGIC{
    'D', 'N', 'C', 'O', 'V', '1'};

// BIP341's recommended NUMS point H. Its discrete logarithm is not known, so
// profile-v1 CTV outputs have no wallet-controlled key-path escape.
constexpr std::array<uint8_t, 32> CTV_NUMS_INTERNAL_KEY{
    0x50, 0x92, 0x9b, 0x74, 0xc1, 0xa0, 0x49, 0x54,
    0xb7, 0x8b, 0x4b, 0x60, 0x35, 0xe9, 0x7a, 0x5e,
    0x07, 0x8a, 0x5a, 0x0f, 0x28, 0xec, 0x96, 0xd5,
    0x47, 0xbf, 0xee, 0x9a, 0xce, 0x80, 0x3a, 0xc0};

void WriteLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

bool ReadLE32(
    const std::vector<uint8_t>& data,
    size_t& offset,
    uint32_t& value) {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    value = static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

void WriteBytes(
    std::vector<uint8_t>& out,
    const std::vector<uint8_t>& bytes) {
    if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("descriptor field exceeds uint32 length");
    }
    WriteLE32(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

bool ReadBytes(
    const std::vector<uint8_t>& data,
    size_t& offset,
    std::vector<uint8_t>& bytes) {
    uint32_t size = 0;
    if (!ReadLE32(data, offset, size) ||
        offset > data.size() ||
        data.size() - offset < size) {
        return false;
    }
    bytes.assign(data.begin() + offset, data.begin() + offset + size);
    offset += size;
    return true;
}

std::array<uint8_t, 32> SHA256(
    const uint8_t* data,
    size_t size) {
    std::array<uint8_t, 32> result{};
    crypto::CSHA256().Write(data, size).Finalize(result.data());
    return result;
}

std::array<uint8_t, 32> SHA256(
    const std::vector<uint8_t>& data) {
    return SHA256(data.data(), data.size());
}

std::string Hex(const uint8_t* data, size_t size) {
    static constexpr char table[] = "0123456789abcdef";
    std::string result;
    result.resize(size * 2);
    for (size_t index = 0; index < size; ++index) {
        result[index * 2] = table[data[index] >> 4];
        result[index * 2 + 1] = table[data[index] & 0x0f];
    }
    return result;
}

uint8_t Nibble(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("descriptor contains non-hex character");
}

std::vector<uint8_t> Unhex(const std::string& hex) {
    if ((hex.size() & 1U) != 0) {
        throw std::invalid_argument("descriptor hex has odd length");
    }
    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        result.push_back(static_cast<uint8_t>(
            (Nibble(hex[index]) << 4) | Nibble(hex[index + 1])));
    }
    return result;
}

std::string EncodeDescriptor(
    ProfileType type,
    const std::vector<uint8_t>& body) {
    std::vector<uint8_t> payload;
    payload.reserve(
        DESCRIPTOR_MAGIC.size() + 2 + body.size() + 32);
    payload.insert(
        payload.end(), DESCRIPTOR_MAGIC.begin(), DESCRIPTOR_MAGIC.end());
    payload.push_back(PROFILE_VERSION);
    payload.push_back(static_cast<uint8_t>(type));
    payload.insert(payload.end(), body.begin(), body.end());
    const auto checksum = SHA256(payload);
    payload.insert(payload.end(), checksum.begin(), checksum.end());
    return std::string(DESCRIPTOR_PREFIX) +
        Hex(payload.data(), payload.size());
}

struct DecodedDescriptor {
    ProfileType type;
    std::vector<uint8_t> body;
    std::string id;
};

DecodedDescriptor DecodeDescriptor(const std::string& descriptor) {
    if (descriptor.rfind(DESCRIPTOR_PREFIX, 0) != 0) {
        throw std::invalid_argument(
            "covenant descriptor must start with dncov1:");
    }
    const auto payload = Unhex(
        descriptor.substr(std::strlen(DESCRIPTOR_PREFIX)));
    // Descriptor ids hash the textual form, so accepting upper-case hex would
    // give one canonical payload multiple ids and let it collide with its own
    // derived scriptPubKey in durable wallet storage.
    if (descriptor != std::string(DESCRIPTOR_PREFIX) +
            Hex(payload.data(), payload.size())) {
        throw std::invalid_argument(
            "covenant descriptor is not canonically encoded");
    }
    constexpr size_t headerSize = DESCRIPTOR_MAGIC.size() + 2;
    constexpr size_t checksumSize = 32;
    if (payload.size() < headerSize + checksumSize ||
        !std::equal(
            DESCRIPTOR_MAGIC.begin(), DESCRIPTOR_MAGIC.end(),
            payload.begin())) {
        throw std::invalid_argument("invalid covenant descriptor header");
    }
    if (payload[DESCRIPTOR_MAGIC.size()] != PROFILE_VERSION) {
        throw std::invalid_argument(
            "unsupported covenant descriptor version");
    }
    const uint8_t rawType = payload[DESCRIPTOR_MAGIC.size() + 1];
    if (rawType != static_cast<uint8_t>(ProfileType::CTV) &&
        rawType != static_cast<uint8_t>(ProfileType::CCV) &&
        rawType != static_cast<uint8_t>(ProfileType::CCV_OWNER)) {
        throw std::invalid_argument("unknown covenant descriptor type");
    }

    const size_t contentSize = payload.size() - checksumSize;
    const auto checksum = SHA256(payload.data(), contentSize);
    if (!std::equal(
            checksum.begin(), checksum.end(),
            payload.begin() + contentSize)) {
        throw std::invalid_argument(
            "covenant descriptor checksum mismatch");
    }

    DecodedDescriptor result;
    result.type = static_cast<ProfileType>(rawType);
    result.body.assign(
        payload.begin() + headerSize, payload.begin() + contentSize);
    const auto idHash = SHA256(
        reinterpret_cast<const uint8_t*>(descriptor.data()),
        descriptor.size());
    result.id = Hex(idHash.data(), idHash.size());
    return result;
}

std::string DescriptorId(const std::string& descriptor) {
    const auto hash = SHA256(
        reinterpret_cast<const uint8_t*>(descriptor.data()),
        descriptor.size());
    return Hex(hash.data(), hash.size());
}

std::array<uint8_t, 32> ToArray(
    const std::vector<uint8_t>& value,
    const char* field) {
    if (value.size() != 32) {
        throw std::runtime_error(std::string(field) + " must be 32 bytes");
    }
    std::array<uint8_t, 32> result{};
    std::copy(value.begin(), value.end(), result.begin());
    return result;
}

TaprootArtifact BuildSingleLeafArtifact(
    const std::array<uint8_t, 32>& internalKey,
    const std::vector<uint8_t>& tapscript) {
    TaprootArtifact result;
    result.internalKey = internalKey;
    result.tapscript = tapscript;
    result.merkleRoot = ToArray(
        consensus::TapLeafHash(TAPSCRIPT_LEAF_VERSION, tapscript),
        "TapLeaf hash");

    std::array<uint8_t, 64> tweakPreimage{};
    std::copy(
        internalKey.begin(), internalKey.end(), tweakPreimage.begin());
    std::copy(
        result.merkleRoot.begin(), result.merkleRoot.end(),
        tweakPreimage.begin() + 32);
    const auto tweak = crypto::TaggedHashArray(
        "TapTweak", tweakPreimage.data(), tweakPreimage.size());

    secp256k1_context* ctx =
        crypto::GetSecp256k1ContextSignVerify();
    secp256k1_xonly_pubkey internalPubkey;
    if (!secp256k1_xonly_pubkey_parse(
            ctx, &internalPubkey, internalKey.data())) {
        throw std::runtime_error("invalid covenant internal key");
    }
    secp256k1_pubkey tweakedPubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(
            ctx, &tweakedPubkey, &internalPubkey, tweak.data())) {
        throw std::runtime_error("invalid covenant TapTweak");
    }
    secp256k1_xonly_pubkey outputKey;
    int parity = 0;
    if (!secp256k1_xonly_pubkey_from_pubkey(
            ctx, &outputKey, &parity, &tweakedPubkey)) {
        throw std::runtime_error("failed to derive covenant output key");
    }
    std::array<uint8_t, 32> serializedOutputKey{};
    if (!secp256k1_xonly_pubkey_serialize(
            ctx, serializedOutputKey.data(), &outputKey)) {
        throw std::runtime_error("failed to serialize covenant output key");
    }
    result.outputKeyParity = static_cast<uint8_t>(parity);
    result.scriptPubKey = {0x51, 0x20};
    result.scriptPubKey.insert(
        result.scriptPubKey.end(),
        serializedOutputKey.begin(), serializedOutputKey.end());
    result.controlBlock = {
        static_cast<uint8_t>(
            TAPSCRIPT_LEAF_VERSION | result.outputKeyParity)};
    result.controlBlock.insert(
        result.controlBlock.end(), internalKey.begin(), internalKey.end());
    return result;
}

std::vector<uint8_t> SerializeContractState(
    const consensus::ContractState& state) {
    if (state.data.size() > consensus::MAX_CONTRACT_STATE_DATA_SIZE) {
        throw std::invalid_argument("CCV state data exceeds profile limit");
    }
    std::vector<uint8_t> result;
    result.reserve(72 + state.data.size());
    result.insert(
        result.end(), state.stateHash.begin(), state.stateHash.end());
    result.insert(
        result.end(), state.codeHash.begin(), state.codeHash.end());
    WriteLE32(result, state.counter);
    WriteLE32(result, static_cast<uint32_t>(state.data.size()));
    result.insert(result.end(), state.data.begin(), state.data.end());
    return result;
}

std::vector<uint8_t> CcvTapscript(
    CCVAuthorization authorization,
    const std::array<uint8_t, 32>& ownerPublicKey) {
    if (authorization == CCVAuthorization::OwnerSchnorr) {
        std::vector<uint8_t> result{
            static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
            32};
        result.insert(
            result.end(), ownerPublicKey.begin(), ownerPublicKey.end());
        result.push_back(static_cast<uint8_t>(consensus::OP_CHECKSIG));
        return result;
    }
    return {
        static_cast<uint8_t>(consensus::OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(consensus::OP_TRUE)};
}

Transaction BuildCTVTemplateTransaction(
    const std::vector<uint32_t>& inputSequences,
    const std::vector<Output>& outputs,
    uint32_t lockTime,
    int32_t version) {
    if (inputSequences.empty()) {
        throw std::invalid_argument("CTV plan requires at least one input");
    }
    if (outputs.empty()) {
        throw std::invalid_argument("CTV plan requires at least one output");
    }
    if (version != Transaction::TX_VERSION_LEGACY &&
        version != Transaction::TX_VERSION_SEGWIT) {
        throw std::invalid_argument(
            "CTV profile requires transparent transaction version 1 or 2");
    }

    Transaction tx;
    tx.version = version;
    tx.lockTime = lockTime;
    for (const uint32_t sequence : inputSequences) {
        TxInput input;
        input.sequence = sequence;
        tx.vin.push_back(std::move(input));
    }
    uint64_t totalOutputValue = 0;
    for (const auto& output : outputs) {
        if (output.scriptPubKey.empty()) {
            throw std::invalid_argument(
                "CTV output scriptPubKey must not be empty");
        }
        if (!output.value.IsPositive() ||
            !output.value.IsWithinSupply()) {
            throw std::invalid_argument(
                "CTV output value is outside the consensus money range");
        }
        if (output.value.GetUna() >
            AmountUna::Max().GetUna() - totalOutputValue) {
            throw std::invalid_argument(
                "CTV output total exceeds the consensus money range");
        }
        totalOutputValue += output.value.GetUna();
        tx.vout.emplace_back(output.value, output.scriptPubKey);
    }
    return tx;
}

CTVPlan CompleteCTVPlan(
    Transaction templateTx,
    uint32_t covenantInputIndex,
    const std::string* recoveredDescriptor) {
    if (templateTx.vin.empty() ||
        templateTx.vout.empty() ||
        covenantInputIndex >= templateTx.vin.size()) {
        throw std::invalid_argument("invalid CTV template dimensions");
    }
    for (const auto& input : templateTx.vin) {
        if (input.prevout.txid.AsUint256() != uint256() ||
            input.prevout.vout != 0 ||
            !input.scriptSig.empty() ||
            !input.witness.empty()) {
            throw std::invalid_argument(
                "CTV descriptor template inputs must use empty zero prevouts");
        }
    }
    if (templateTx.HasConfidentialOutputs() ||
        templateTx.has_explicit_fee ||
        Transaction::IsShieldedVersion(templateTx.version)) {
        throw std::invalid_argument(
            "CTV descriptor is outside the transparent BIP119 profile");
    }

    CTVPlan result;
    result.templateTx = std::move(templateTx);
    result.covenantInputIndex = covenantInputIndex;
    result.templateHash = consensus::ComputeCTVHash(
        result.templateTx, covenantInputIndex);
    result.taproot.tapscript = {32};
    result.taproot.tapscript.insert(
        result.taproot.tapscript.end(),
        result.templateHash.begin(), result.templateHash.end());
    result.taproot.tapscript.push_back(
        static_cast<uint8_t>(
            consensus::OP_CHECKTEMPLATEVERIFY));
    result.taproot = BuildSingleLeafArtifact(
        CTV_NUMS_INTERNAL_KEY, result.taproot.tapscript);

    if (recoveredDescriptor != nullptr) {
        result.recoveryDescriptor = *recoveredDescriptor;
    } else {
        std::vector<uint8_t> body;
        WriteLE32(body, covenantInputIndex);
        WriteBytes(
            body,
            result.templateTx.Serialize(
                TxSerializationMode::WithoutWitness));
        result.recoveryDescriptor =
            EncodeDescriptor(ProfileType::CTV, body);
    }
    result.descriptorId = DescriptorId(result.recoveryDescriptor);
    return result;
}

CCVPlan CompleteCCVPlan(
    ProfileType type,
    uint32_t counter,
    const std::vector<uint8_t>& data,
    const std::array<uint8_t, 32>& ownerPublicKey,
    const std::string& ownerKeyOrigin,
    const std::string* recoveredDescriptor) {
    if (data.size() > consensus::MAX_CONTRACT_STATE_DATA_SIZE) {
        throw std::invalid_argument("CCV state data exceeds profile limit");
    }

    CCVPlan result;
    if (type != ProfileType::CCV && type != ProfileType::CCV_OWNER) {
        throw std::invalid_argument("descriptor is not a CCV profile");
    }
    result.authorization =
        type == ProfileType::CCV_OWNER
            ? CCVAuthorization::OwnerSchnorr
            : CCVAuthorization::Permissionless;
    result.ownerPublicKey = ownerPublicKey;
    result.ownerKeyOrigin = ownerKeyOrigin;
    if (result.authorization == CCVAuthorization::OwnerSchnorr) {
        secp256k1_xonly_pubkey parsed;
        if (!secp256k1_xonly_pubkey_parse(
                crypto::GetSecp256k1ContextSignVerify(),
                &parsed,
                result.ownerPublicKey.data())) {
            throw std::invalid_argument("invalid CCV owner x-only public key");
        }
        const auto parsedOrigin =
            wallet::KeyOriginInfo::parsePathString(result.ownerKeyOrigin);
        if (result.ownerKeyOrigin.size() > 255 ||
            !parsedOrigin.has_value() ||
            parsedOrigin->getPathString() != result.ownerKeyOrigin) {
            throw std::invalid_argument(
                "owner-authorized CCV requires a canonical BIP32 key origin");
        }
    } else if (!result.ownerKeyOrigin.empty() ||
               std::any_of(
                   result.ownerPublicKey.begin(),
                   result.ownerPublicKey.end(),
                   [](uint8_t value) { return value != 0; })) {
        throw std::invalid_argument(
            "permissionless CCV must not contain owner metadata");
    }
    result.state.counter = counter;
    result.state.data = data;
    result.taproot.tapscript = CcvTapscript(
        result.authorization, result.ownerPublicKey);
    result.state.codeHash =
        consensus::ComputeContractCodeHash(result.taproot.tapscript);
    result.state.stateHash =
        consensus::ComputeContractStateHash(result.state);
    result.taproot.merkleRoot = ToArray(
        consensus::TapLeafHash(
            TAPSCRIPT_LEAF_VERSION, result.taproot.tapscript),
        "TapLeaf hash");
    if (!consensus::DeriveContractInternalKey(
            result.state, result.taproot.internalKey) ||
        !consensus::ComputeContractOutputScript(
            result.state,
            result.taproot.merkleRoot,
            result.taproot.scriptPubKey,
            &result.taproot.outputKeyParity)) {
        throw std::runtime_error("failed to derive CCV output");
    }
    result.taproot.controlBlock = {
        static_cast<uint8_t>(
            TAPSCRIPT_LEAF_VERSION |
            result.taproot.outputKeyParity)};
    result.taproot.controlBlock.insert(
        result.taproot.controlBlock.end(),
        result.taproot.internalKey.begin(),
        result.taproot.internalKey.end());

    if (recoveredDescriptor != nullptr) {
        result.recoveryDescriptor = *recoveredDescriptor;
    } else {
        std::vector<uint8_t> body;
        if (result.authorization == CCVAuthorization::OwnerSchnorr) {
            body.insert(
                body.end(),
                result.ownerPublicKey.begin(),
                result.ownerPublicKey.end());
            WriteBytes(
                body,
                std::vector<uint8_t>(
                    result.ownerKeyOrigin.begin(),
                    result.ownerKeyOrigin.end()));
        }
        WriteLE32(body, counter);
        WriteBytes(body, data);
        result.recoveryDescriptor =
            EncodeDescriptor(type, body);
    }
    result.descriptorId = DescriptorId(result.recoveryDescriptor);
    return result;
}

} // namespace

CTVPlan BuildCTVPlan(
    const std::vector<uint32_t>& inputSequences,
    uint32_t covenantInputIndex,
    const std::vector<Output>& outputs,
    uint32_t lockTime,
    int32_t version) {
    if (covenantInputIndex >= inputSequences.size()) {
        throw std::invalid_argument(
            "CTV covenant input index is out of range");
    }
    return CompleteCTVPlan(
        BuildCTVTemplateTransaction(
            inputSequences, outputs, lockTime, version),
        covenantInputIndex,
        nullptr);
}

CTVPlan RecoverCTVPlan(const std::string& descriptor) {
    const auto decoded = DecodeDescriptor(descriptor);
    if (decoded.type != ProfileType::CTV) {
        throw std::invalid_argument("descriptor is not a CTV profile");
    }
    size_t offset = 0;
    uint32_t inputIndex = 0;
    std::vector<uint8_t> txBytes;
    if (!ReadLE32(decoded.body, offset, inputIndex) ||
        !ReadBytes(decoded.body, offset, txBytes) ||
        offset != decoded.body.size()) {
        throw std::invalid_argument("malformed CTV descriptor body");
    }
    Transaction templateTx;
    size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(
            templateTx, txBytes, consumed) ||
        consumed != txBytes.size() ||
        templateTx.Serialize(TxSerializationMode::WithoutWitness) != txBytes) {
        throw std::invalid_argument(
            "CTV descriptor contains non-canonical transaction");
    }
    return CompleteCTVPlan(
        std::move(templateTx), inputIndex, &descriptor);
}

Transaction BuildCTVSpend(
    const CTVPlan& plan,
    const std::vector<TxOutPoint>& prevouts) {
    const CTVPlan recovered =
        RecoverCTVPlan(plan.recoveryDescriptor);
    if (recovered.templateHash != plan.templateHash ||
        recovered.taproot.scriptPubKey != plan.taproot.scriptPubKey ||
        recovered.covenantInputIndex != plan.covenantInputIndex) {
        throw std::invalid_argument(
            "CTV plan does not match its recovery descriptor");
    }
    if (prevouts.size() != recovered.templateTx.vin.size()) {
        throw std::invalid_argument(
            "CTV prevout count does not match committed input count");
    }

    Transaction tx = recovered.templateTx;
    std::set<TxOutPoint> uniquePrevouts;
    for (size_t index = 0; index < prevouts.size(); ++index) {
        if (!uniquePrevouts.insert(prevouts[index]).second) {
            throw std::invalid_argument(
                "CTV spend contains a duplicate prevout");
        }
        tx.vin[index].prevout = prevouts[index];
    }
    // The descriptor intentionally stores a witnessless template. Its
    // canonical deserialization therefore marks the transaction legacy.
    // Restore the actual spend's witness version before serialization or the
    // valid in-memory Taproot witness would be omitted from RPC/P2P bytes.
    tx.witness_version = 1;
    tx.vin[recovered.covenantInputIndex].witness = {
        recovered.taproot.tapscript,
        recovered.taproot.controlBlock};

    std::array<uint8_t, 32> spendHash{};
    if (!consensus::TryComputeCTVHash(
            tx, recovered.covenantInputIndex, spendHash) ||
        spendHash != recovered.templateHash) {
        throw std::runtime_error(
            "constructed CTV spend does not match its template");
    }
    return tx;
}

CCVPlan BuildCCVPlan(
    uint32_t counter,
    const std::vector<uint8_t>& data) {
    return CompleteCCVPlan(
        ProfileType::CCV, counter, data, {}, {}, nullptr);
}

CCVPlan BuildOwnerAuthorizedCCVPlan(
    uint32_t counter,
    const std::vector<uint8_t>& data,
    const std::array<uint8_t, 32>& ownerPublicKey,
    const std::string& ownerKeyOrigin) {
    return CompleteCCVPlan(
        ProfileType::CCV_OWNER,
        counter,
        data,
        ownerPublicKey,
        ownerKeyOrigin,
        nullptr);
}

std::array<uint8_t, 32> OwnerXOnlyPublicKey(
    const std::vector<uint8_t>& privateKey) {
    if (privateKey.size() != 32) {
        throw std::invalid_argument("CCV owner private key must be 32 bytes");
    }
    secp256k1_context* context = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_keypair keypair;
    secp256k1_xonly_pubkey publicKey;
    int parity = 0;
    std::array<uint8_t, 32> result{};
    if (!secp256k1_keypair_create(
            context, &keypair, privateKey.data()) ||
        !secp256k1_keypair_xonly_pub(
            context, &publicKey, &parity, &keypair) ||
        !secp256k1_xonly_pubkey_serialize(
            context, result.data(), &publicKey)) {
        throw std::invalid_argument("invalid CCV owner private key");
    }
    return result;
}

CCVPlan RecoverCCVPlan(const std::string& descriptor) {
    const auto decoded = DecodeDescriptor(descriptor);
    if (decoded.type != ProfileType::CCV &&
        decoded.type != ProfileType::CCV_OWNER) {
        throw std::invalid_argument("descriptor is not a CCV profile");
    }
    size_t offset = 0;
    std::array<uint8_t, 32> ownerPublicKey{};
    std::string ownerKeyOrigin;
    if (decoded.type == ProfileType::CCV_OWNER) {
        if (decoded.body.size() < ownerPublicKey.size()) {
            throw std::invalid_argument("malformed owner CCV descriptor body");
        }
        std::copy_n(
            decoded.body.begin(), ownerPublicKey.size(), ownerPublicKey.begin());
        offset += ownerPublicKey.size();
        std::vector<uint8_t> encodedOrigin;
        if (!ReadBytes(decoded.body, offset, encodedOrigin)) {
            throw std::invalid_argument("malformed owner CCV key origin");
        }
        ownerKeyOrigin.assign(encodedOrigin.begin(), encodedOrigin.end());
    }
    uint32_t counter = 0;
    std::vector<uint8_t> data;
    if (!ReadLE32(decoded.body, offset, counter) ||
        !ReadBytes(decoded.body, offset, data) ||
        offset != decoded.body.size()) {
        throw std::invalid_argument("malformed CCV descriptor body");
    }
    return CompleteCCVPlan(
        decoded.type,
        counter,
        data,
        ownerPublicKey,
        ownerKeyOrigin,
        &descriptor);
}

namespace {

CCVTransition BuildCCVTransitionCommon(
    const CCVPlan& recovered,
    const std::vector<Input>& inputs,
    AmountUna covenantValue,
    const std::vector<uint8_t>& nextData,
    const std::vector<Output>& additionalOutputs,
    uint32_t lockTime,
    int32_t version) {
    if (inputs.empty()) {
        throw std::invalid_argument(
            "CCV transition requires the covenant input at index zero");
    }
    if (recovered.state.counter == std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("CCV counter cannot advance");
    }
    if (version != Transaction::TX_VERSION_LEGACY &&
        version != Transaction::TX_VERSION_SEGWIT) {
        throw std::invalid_argument(
            "CCV profile requires transparent transaction version 1 or 2");
    }
    if (!covenantValue.IsPositive() ||
        !covenantValue.IsWithinSupply()) {
        throw std::invalid_argument(
            "CCV covenant value is outside the consensus money range");
    }

    CCVTransition result;
    result.successor =
        recovered.authorization == CCVAuthorization::OwnerSchnorr
            ? BuildOwnerAuthorizedCCVPlan(
                  recovered.state.counter + 1,
                  nextData,
                  recovered.ownerPublicKey,
                  recovered.ownerKeyOrigin)
            : BuildCCVPlan(recovered.state.counter + 1, nextData);
    result.tx.version = version;
    result.tx.witness_version = 1;
    result.tx.lockTime = lockTime;
    std::set<TxOutPoint> uniquePrevouts;
    for (const auto& source : inputs) {
        if (!uniquePrevouts.insert(source.prevout).second) {
            throw std::invalid_argument(
                "CCV transition contains a duplicate prevout");
        }
        TxInput input;
        input.prevout = source.prevout;
        input.sequence = source.sequence;
        result.tx.vin.push_back(std::move(input));
    }
    result.tx.vout.emplace_back(
        covenantValue, result.successor.taproot.scriptPubKey);
    uint64_t totalOutputValue = covenantValue.GetUna();
    for (const auto& output : additionalOutputs) {
        if (output.scriptPubKey.empty()) {
            throw std::invalid_argument(
                "CCV additional output scriptPubKey must not be empty");
        }
        if (!output.value.IsPositive() ||
            !output.value.IsWithinSupply()) {
            throw std::invalid_argument(
                "CCV additional output value is outside the consensus money range");
        }
        if (output.value.GetUna() >
            AmountUna::Max().GetUna() - totalOutputValue) {
            throw std::invalid_argument(
                "CCV output total exceeds the consensus money range");
        }
        totalOutputValue += output.value.GetUna();
        result.tx.vout.emplace_back(output.value, output.scriptPubKey);
    }
    result.tx.vin[0].witness = {
        SerializeContractState(recovered.state),
        SerializeContractState(result.successor.state),
        recovered.taproot.tapscript,
        recovered.taproot.controlBlock};
    return result;
}

} // namespace

CCVTransition BuildCCVTransition(
    const CCVPlan& current,
    const std::vector<Input>& inputs,
    AmountUna covenantValue,
    const std::vector<uint8_t>& nextData,
    const std::vector<Output>& additionalOutputs,
    uint32_t lockTime,
    int32_t version) {
    const CCVPlan recovered =
        RecoverCCVPlan(current.recoveryDescriptor);
    if (recovered.state.stateHash != current.state.stateHash ||
        recovered.taproot.scriptPubKey != current.taproot.scriptPubKey) {
        throw std::invalid_argument(
            "CCV plan does not match its recovery descriptor");
    }
    if (recovered.authorization != CCVAuthorization::Permissionless) {
        throw std::invalid_argument(
            "owner-authorized CCV requires the signing transition builder");
    }
    return BuildCCVTransitionCommon(
        recovered,
        inputs,
        covenantValue,
        nextData,
        additionalOutputs,
        lockTime,
        version);
}

CCVTransition BuildOwnerAuthorizedCCVTransition(
    const CCVPlan& current,
    const std::vector<Input>& inputs,
    const std::vector<Prevout>& prevouts,
    AmountUna covenantValue,
    const std::vector<uint8_t>& nextData,
    const std::vector<uint8_t>& ownerPrivateKey,
    const std::vector<Output>& additionalOutputs,
    uint32_t lockTime,
    int32_t version) {
    const CCVPlan recovered = RecoverCCVPlan(current.recoveryDescriptor);
    if (recovered.state.stateHash != current.state.stateHash ||
        recovered.taproot.scriptPubKey != current.taproot.scriptPubKey ||
        recovered.authorization != CCVAuthorization::OwnerSchnorr) {
        throw std::invalid_argument(
            "owner CCV plan does not match its recovery descriptor");
    }
    if (OwnerXOnlyPublicKey(ownerPrivateKey) != recovered.ownerPublicKey) {
        throw std::invalid_argument(
            "CCV owner private key does not match recovery descriptor");
    }
    if (prevouts.size() != inputs.size() || prevouts.empty()) {
        throw std::invalid_argument(
            "owner CCV requires prevout metadata for every input");
    }
    if (prevouts[0].value != covenantValue ||
        prevouts[0].scriptPubKey != recovered.taproot.scriptPubKey) {
        throw std::invalid_argument(
            "owner CCV covenant prevout does not match current state");
    }
    std::vector<CanonicalWalletUTXO> signingPrevouts;
    signingPrevouts.reserve(prevouts.size());
    for (size_t index = 0; index < prevouts.size(); ++index) {
        if (!prevouts[index].value.IsPositive() ||
            !prevouts[index].value.IsWithinSupply() ||
            prevouts[index].scriptPubKey.empty()) {
            throw std::invalid_argument("invalid CCV signing prevout metadata");
        }
        CanonicalWalletUTXO utxo;
        utxo.txid = inputs[index].prevout.txid.AsUint256();
        utxo.vout = inputs[index].prevout.vout;
        utxo.value = prevouts[index].value;
        utxo.spk = prevouts[index].scriptPubKey;
        signingPrevouts.push_back(std::move(utxo));
    }

    CCVTransition result = BuildCCVTransitionCommon(
        recovered,
        inputs,
        covenantValue,
        nextData,
        additionalOutputs,
        lockTime,
        version);
    const auto sighash = TaprootTxSigner::ComputeScriptPathSighash(
        result.tx,
        0,
        signingPrevouts,
        recovered.taproot.tapscript,
        TAPSCRIPT_LEAF_VERSION,
        TaprootTxSigner::SIGHASH_DEFAULT);
    const auto signature =
        TaprootTxSigner::SignSchnorr(sighash, ownerPrivateKey);
    if (signature.size() != 64) {
        throw std::runtime_error("failed to sign owner-authorized CCV transition");
    }
    result.tx.vin[0].witness.insert(
        result.tx.vin[0].witness.begin(), signature);
    return result;
}

ProfileType DescriptorType(const std::string& descriptor) {
    return DecodeDescriptor(descriptor).type;
}

} // namespace dinero::wallet::covenant
