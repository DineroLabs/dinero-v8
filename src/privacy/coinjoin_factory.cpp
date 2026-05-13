#include "privacy/coinjoin_factory.h"
#include "privacy/coinjoin_adapter_generic.h"
#include "privacy/coinjoin_adapter_jm.h"

namespace din {

// Adapter factory implementation
std::unique_ptr<ICoinJoinAdapter> make_cj_adapter(const std::string& type, const std::string& base_url) {
    if (type == "generic") {
        return std::make_unique<CoinJoinAdapterGeneric>(base_url);
    }
    if (type == "jm") {
        return std::make_unique<CoinJoinAdapterJM>(base_url);
    }
    throw std::runtime_error("Unknown coinjoin adapter type: " + type);
}

} // namespace din
