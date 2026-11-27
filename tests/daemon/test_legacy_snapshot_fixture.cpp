#include "consensus/chainparams.h"
#include "daemon/genesis_init.hpp"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "wallet/utxo_index.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected an isolated data directory");
        dinero::SelectParams(dinero::Chain::MAINNET);
        const std::filesystem::path blockchain = std::filesystem::path(argv[1]) / "blockchain";
        std::filesystem::create_directories(blockchain);

        dinero::ChainDB chain;
        if (chain.init(blockchain / "chaindb") != dinero::Status::Ok)
            throw std::runtime_error("could not initialize mainnet ChainDB");
        dinero::BlockStorage blocks;
        if (blocks.init(blockchain) != dinero::Status::Ok ||
            !dinero::InitializeGenesis(&chain, &blocks, nullptr))
            throw std::runtime_error("could not initialize mainnet genesis");
        dinero::UTXOIndex index((blockchain / "utxo").string());
        if (!index.Initialize()) throw std::runtime_error("could not initialize v2 wallet index");
        std::cout << "PASS isolated mainnet v2 wallet index fixture\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
