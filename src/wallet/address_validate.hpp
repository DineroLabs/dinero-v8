#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct DnrAddressInfo {
    bool        ok = false;
    std::string error;

    // Only set when ok==true
    int                     witness_version = -1;   // 0..16
    std::vector<uint8_t>    program;               // witness program bytes
    std::string             program_hex;           // hex(program)
    std::string             script_pubkey_hex;     // hex(scriptPubKey)
    std::string             type;                  // "witness_v0_keyhash" | "witness_v0_scripthash" | "witness_unknown"
};

bool IsValidDnrAddress(const std::string& addr, DnrAddressInfo& out);
