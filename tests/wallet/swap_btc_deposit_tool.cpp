// Builds deposit transactions for a real node to judge
// (tests/wallet/swap_btc_deposit_regtest.sh):
//   swap_btc_deposit_tool address <key hex> <hrp>
//   swap_btc_deposit_tool lock  <key hex> <coins> <lock script hex> <amount sat> <change script hex> <sat/vB> <max fee>
//   swap_btc_deposit_tool sweep <key hex> <coins> <to script hex> <sat/vB> <max fee>
// <coins> is txid:vout:value[,txid:vout:value…] with txids as Bitcoin prints them.
// lock/sweep print "<raw hex> <txid> <fee> <vsize> <inputs>".

#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/btc_header_chain.h"
#include "wallet/swap/swap_crypto.h"

#include <iostream>
#include <sstream>

using namespace dinero::swap;

namespace {
std::vector<uint8_t> Unhex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}
Bytes32 KeyOf(const std::string& hex) {
    const auto bytes = Unhex(hex);
    Bytes32 key{};
    if (bytes.size() != 32) throw std::invalid_argument("key must be 32 bytes of hex");
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}
std::vector<BtcDepositCoin> Coins(const std::string& text) {
    std::vector<BtcDepositCoin> coins;
    std::stringstream in(text);
    for (std::string item; std::getline(in, item, ',');) {
        const auto a = item.find(':'), b = item.rfind(':');
        BtcDepositCoin c;
        c.txid = *BtcHashFromHex(item.substr(0, a));
        c.vout = static_cast<uint32_t>(std::stoul(item.substr(a + 1, b - a - 1)));
        c.value_sat = std::stoull(item.substr(b + 1));
        coins.push_back(c);
    }
    return coins;
}
void Print(const BtcDepositSpend& plan) {
    std::cout << detail::ToHex(plan.raw) << " " << BtcHashToHex(plan.txid) << " " << plan.fee_sat << " " << plan.vsize << " "
              << plan.used.size() << "\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "address" && argc == 4) {
            std::cout << BtcP2wpkhAddress(detail::CompressedOf(KeyOf(argv[2])), argv[3]) << "\n";
            return 0;
        }
        if (mode == "lock" && argc == 9) {
            Print(BuildBtcLockFromDeposit(Coins(argv[3]), KeyOf(argv[2]), Unhex(argv[4]), std::stoull(argv[5]), Unhex(argv[6]),
                                          std::stod(argv[7]), std::stoull(argv[8])));
            return 0;
        }
        if (mode == "sweep" && argc == 7) {
            Print(BuildBtcDepositSweep(Coins(argv[3]), KeyOf(argv[2]), Unhex(argv[4]), std::stod(argv[5]), std::stoull(argv[6])));
            return 0;
        }
    } catch (const std::exception& e) {
        std::cout << "error " << e.what() << "\n";
        return 1;
    }
    std::cerr << "usage: swap_btc_deposit_tool address|lock|sweep ...\n";
    return 2;
}
