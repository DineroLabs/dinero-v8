#include "primitives/transaction.h"
#include <iostream>

// Always built, including backend-OFF builds. The marker family must never be
// consumed as a ten-byte empty historical transaction in a block stream.
int main() {
    using namespace dinero;
    const std::vector<uint8_t> marked{7,0,0,0,0,0,'D','N','O','R','C','H','T','X',1,0,0,0,0};
    Transaction tx; tx.lockTime=123; size_t consumed=999;
    if(TransactionSerializer::Deserialize(tx,marked,consumed) || consumed!=0 || tx.lockTime!=123) {
        std::cerr<<"marked Orchard family reached historical parser\n"; return 1;
    }
    // A version number alone must not reserve historically ordinary v7 bytes.
    Transaction ordinary; ordinary.version=7;
    ordinary.vin.emplace_back(); ordinary.vin[0].prevout.vout=3;
    ordinary.vout.emplace_back(AmountUna::Una(1),std::vector<uint8_t>{0x51});
    auto bytes=ordinary.Serialize(TxSerializationMode::WithWitness);
    if(!TransactionSerializer::Deserialize(tx,bytes,consumed) || consumed!=bytes.size() ||
       tx.Serialize(TxSerializationMode::WithWitness)!=bytes) {
        std::cerr<<"ordinary v7 changed\n"; return 1;
    }
    std::cout<<"Historical parser rejects Orchard marker and preserves ordinary v7\n";
}
