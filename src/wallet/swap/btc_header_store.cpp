#include "wallet/swap/btc_header_store.h"

#include "wallet/swap/swap_crypto.h"

#include <fstream>
#include <iterator>

namespace dinero::swap {
namespace {

// "DINBTCH1" + network name + 0 + checkpoint height (4, LE) + checkpoint hash (32).
std::string Label(const BtcHeaderChain& chain) {
    const BtcChainParams& p = chain.params();
    std::string label = "DINBTCH1";
    label += p.name;
    label.push_back('\0');
    for (int i = 0; i < 4; ++i) label.push_back(static_cast<char>(p.checkpoint_height >> (8 * i)));
    const Bytes32 hash = BtcBlockHash(p.checkpoint);
    label.append(hash.begin(), hash.end());
    return label;
}

}  // namespace

void SaveBtcHeaderChain(const std::string& path, const BtcHeaderChain& chain) {
    std::string file = Label(chain);
    const auto headers = chain.SerializeBestChain();
    file.append(headers.begin(), headers.end());
    detail::WriteFileAtomically(path, file);
}

size_t LoadBtcHeaderChain(const std::string& path, BtcHeaderChain& chain, uint32_t now_unix) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string label = Label(chain);
    if (file.size() < label.size() || file.compare(0, label.size(), label) != 0) return 0;
    return chain.LoadBestChain({file.begin() + label.size(), file.end()}, now_unix);
}

}  // namespace dinero::swap
