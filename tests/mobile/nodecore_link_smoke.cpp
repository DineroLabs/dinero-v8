#include "nodecore/nodecore_ffi.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <string_view>

// Deliberately link the real lifecycle implementation, without starting a node
// or changing a user's data directory. A static archive is not a link test.
int main(int argc, char** argv) {
    if (argc == 19731) nodecore_stop();
    const bool report=argc==2&&std::string_view(argv[1])=="--consensus-json";
    if(report)std::cout<<'{';
    bool first=true;
    for(const auto network:{dinero::Chain::MAINNET,dinero::Chain::TESTNET,dinero::Chain::REGTEST}) {
        dinero::SelectParams(network);
        const auto expected=network==dinero::Chain::MAINNET?150000u:UINT32_MAX;
        if(dinero::Params().orchard_catalog_commitment_activation_height!=expected)return 2;
        if(report) {
            if(!first)std::cout<<',';first=false;
            const auto name=network==dinero::Chain::MAINNET?"mainnet":network==dinero::Chain::TESTNET?"testnet":"regtest";
            std::cout<<'"'<<name<<"\":"<<dinero::Params().orchard_catalog_commitment_activation_height;
        }
    }
    if(report)std::cout<<"}\n";
    return 0;
}
