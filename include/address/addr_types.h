#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

struct Destination {
    bool is_valid = false;
    std::vector<uint8_t> pubkey_hash;
    std::string address_string;
    
    // Constructor
    Destination() = default;
    Destination(const std::vector<uint8_t>& hash) : is_valid(true), pubkey_hash(hash) {}
};

enum class AddrType {
    Base58,
    Bech32
};

struct ParsedAddress {
    Destination dest;
    AddrType type;
};

bool IsValidDestination(const Destination& d);

} // namespace dinero
