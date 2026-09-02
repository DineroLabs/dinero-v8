#include "consensus/shielded/range_proof.h"
#include "consensus/shielded/shielded_serialization.h"
#include "wallet/shielded_derivation.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace dinero::consensus::shielded;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t selector = data[0] % 4;
    ++data;
    --size;
    if (selector == 0) {
        ShieldedBundle bundle;
        (void)DeserializeShieldedBundle(data, size, &bundle);
    } else if (selector == 1) {
        ShieldedBundle bundle;
        bundle.aggregated_range_proof.assign(data, data + size);
        (void)VerifyBundleRangeProofs(bundle);  // exercises bounded container decoder
    } else if (selector == 2) {
        const size_t capped = std::min<size_t>(size, 512);
        try {
            (void)dinero::wallet::shielded::DecodeShieldedAddress(
                std::string(reinterpret_cast<const char*>(data), capped));
        } catch (...) {
        }
    } else if (size == dinero::wallet::shielded::kEncryptedNoteBytes) {
        dinero::wallet::shielded::EncryptedNote encrypted{};
        std::copy(data, data + size, encrypted.begin());
        dinero::wallet::shielded::Hash ivk{};
        if (size >= ivk.size()) std::copy(data, data + ivk.size(), ivk.begin());
        (void)dinero::wallet::shielded::TryDecryptNoteForViewer(ivk, encrypted);
    }
    return 0;
}
