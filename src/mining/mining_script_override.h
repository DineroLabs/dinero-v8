#pragma once
#include <vector>
#include <cstdint>

namespace dinero {

enum class MiningAddressType {
    TRANSPARENT = 0,
    TAPROOT = 1,
    CONFIDENTIAL = 2
};

extern bool g_mining_override_active;
extern int g_mining_override_witver;
extern std::vector<uint8_t> g_mining_override_witprog;
extern MiningAddressType g_mining_override_type;

}  // namespace dinero
