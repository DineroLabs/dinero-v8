#include "nodecore/nodecore_ffi.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <string_view>

// Deliberately link the real lifecycle implementation, without starting a node
// or changing a user's data directory. A static archive is not a link test.
int main(int argc, char** argv) {
    if (argc == 19731) nodecore_stop();
    const bool report_orchard=argc==2&&std::string_view(argv[1])=="--consensus-json";
    const bool report_h1=argc==2&&std::string_view(argv[1])=="--coinbase-structure-json";
    const bool report=report_orchard||report_h1;
    if(report)std::cout<<'{';
    bool first=true;
    for(const auto network:{dinero::Chain::MAINNET,dinero::Chain::TESTNET,dinero::Chain::REGTEST}) {
        dinero::SelectParams(network);
        const auto expected=network==dinero::Chain::MAINNET?150000u:UINT32_MAX;
        if(dinero::Params().orchard_catalog_commitment_activation_height!=expected)return 2;
        const auto h1_expected=network==dinero::Chain::TESTNET?UINT32_MAX:1u;
        if(dinero::Params().coinbase_structure_activation_height!=h1_expected)return 3;
        if(report) {
            if(!first)std::cout<<',';first=false;
            const auto name=network==dinero::Chain::MAINNET?"mainnet":network==dinero::Chain::TESTNET?"testnet":"regtest";
            const auto height=report_h1?dinero::Params().coinbase_structure_activation_height:
                                        dinero::Params().orchard_catalog_commitment_activation_height;
            std::cout<<'"'<<name<<"\":"<<height;
        }
    }
    if(report)std::cout<<"}\n";
    return 0;
}
