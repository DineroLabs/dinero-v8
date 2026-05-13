#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Forward declare Network enum from chainparams
enum class Network {
    MAIN,
    TEST,
    REGTEST
};

enum class AddrType {
    Unknown,
    P2PKH,      // Pay-to-Public-Key-Hash (legacy)
    P2SH,       // Pay-to-Script-Hash (legacy)
    P2WPKH,     // Pay-to-Witness-Public-Key-Hash (segwit v0)
    P2WSH,      // Pay-to-Witness-Script-Hash (segwit v0)
    P2TR        // Pay-to-Taproot (segwit v1)
};

struct DecodedAddr {
    AddrType type;
    Network network;
    std::vector<uint8_t> data;
    
    DecodedAddr() : type(AddrType::Unknown), network(Network::MAIN) {}
    DecodedAddr(AddrType t, Network n, const std::vector<uint8_t>& d) 
        : type(t), network(n), data(d) {}
};

class AddressCodec {
public:
    // Decode any address format (Bech32, Bech32m, Base58Check)
    static DecodedAddr decode(const std::string& address);
    
    // Encode addresses in various formats
    static std::string encodeP2WPKH(Network net, const std::vector<uint8_t>& pubkey_hash);
    static std::string encodeP2TR(Network net, const std::vector<uint8_t>& taproot_output);
    static std::string encodeP2PKH(Network net, const std::vector<uint8_t>& pubkey_hash);
    static std::string encodeP2SH(Network net, const std::vector<uint8_t>& script_hash);
};
