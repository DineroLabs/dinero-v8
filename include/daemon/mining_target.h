#pragma once

#include <vector>
#include <string>

namespace dinero {

// Mining target structure to store witness data and avoid re-decoding
struct MiningTarget {
    int witver = -1;                       // 0..16 or -1 if unset
    std::vector<uint8_t> witprog;          // 20 (P2WPKH) or 32 (P2WSH)
    std::string hrp;                       // "rdin"/"din"/"tdin"
    std::string addr;                      // optional: display/log only

    bool has_witness() const {
        return witver >= 0 && (witprog.size() == 20 || witprog.size() == 32);
    }
    
    // Clear all data
    void clear() {
        witver = -1;
        witprog.clear();
        hrp.clear();
        addr.clear();
    }
    
    // Set witness data
    void set_witness(int version, const std::vector<uint8_t>& program, const std::string& network_hrp) {
        witver = version;
        witprog = program;
        hrp = network_hrp;
    }
    
    // Check if this is a valid P2WPKH target
    bool is_p2wpkh() const {
        return witver == 0 && witprog.size() == 20;
    }
    
    // Check if this is a valid P2WSH target
    bool is_p2wsh() const {
        return witver == 0 && witprog.size() == 32;
    }
};

} // namespace dinero
