/**
 * V7 P2MR address codec — thin wrapper over the existing Bech32Encoder.
 *
 * The only novelty vs. the existing Taproot (witness v1) path is the
 * witness-version number (3) and the strict 32-byte program length check.
 */

#include "wallet/p2mr_address.h"

#include "daemon/bech32_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::wallet {

namespace {

bool HrpLooksValid(const std::string& hrp) {
    if (hrp.empty() || hrp.size() > 83) {
        return false;  // bech32 HRP length bounds
    }
    for (char c : hrp) {
        // bech32 allows US-ASCII 33..126 in HRP, but in practice we want
        // lowercase alphanum to match the rest of the chain's address
        // prefixes ("din" / "tdin" / "rdin"). Reject anything else to keep
        // the codec predictable.
        if (c < 'a' || c > 'z') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string EncodeP2MRAddress(const std::string& hrp,
                              const std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES>& merkle_root) {
    if (!HrpLooksValid(hrp)) {
        return {};
    }
    std::vector<uint8_t> program(merkle_root.begin(), merkle_root.end());
    return Bech32Encoder::encode_segwit_address(hrp, P2MR_WITNESS_VERSION, program);
}

std::string EncodeP2MRAddress(const std::string& hrp,
                              const std::vector<uint8_t>& merkle_root) {
    if (merkle_root.size() != P2MR_MERKLE_ROOT_BYTES) {
        return {};
    }
    std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES> arr{};
    for (std::size_t i = 0; i < P2MR_MERKLE_ROOT_BYTES; ++i) {
        arr[i] = merkle_root[i];
    }
    return EncodeP2MRAddress(hrp, arr);
}

std::optional<DecodedP2MR> DecodeP2MRAddress(const std::string& address) {
    auto decoded = Bech32Encoder::decode_segwit_address(address);
    if (!decoded.valid) {
        return std::nullopt;
    }
    if (decoded.witness_version != P2MR_WITNESS_VERSION) {
        return std::nullopt;
    }
    if (decoded.witness_program.size() != P2MR_MERKLE_ROOT_BYTES) {
        return std::nullopt;
    }

    DecodedP2MR out;
    out.hrp = decoded.hrp;
    for (std::size_t i = 0; i < P2MR_MERKLE_ROOT_BYTES; ++i) {
        out.merkle_root[i] = decoded.witness_program[i];
    }
    return out;
}

std::vector<uint8_t> BuildP2MRScriptPubKey(
    const std::array<uint8_t, P2MR_MERKLE_ROOT_BYTES>& merkle_root) {
    std::vector<uint8_t> script;
    script.reserve(34);
    script.push_back(0x53);  // OP_3 (witness version 3)
    script.push_back(0x20);  // PUSH32
    script.insert(script.end(), merkle_root.begin(), merkle_root.end());
    return script;
}

} // namespace dinero::wallet
