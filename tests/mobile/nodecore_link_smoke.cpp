#include "nodecore/nodecore_ffi.h"
#include "consensus/chainparams.h"

// Deliberately link the real lifecycle implementation, without starting a node
// or changing a user's data directory. A static archive is not a link test.
int main(int argc, char**) {
    if (argc == 19731) nodecore_stop();
    for(const auto network:{dinero::Chain::MAINNET,dinero::Chain::TESTNET,dinero::Chain::REGTEST}) {
        dinero::SelectParams(network);
        const auto expected=network==dinero::Chain::MAINNET?150000u:UINT32_MAX;
        if(dinero::Params().orchard_catalog_commitment_activation_height!=expected)return 2;
    }
    return 0;
}
