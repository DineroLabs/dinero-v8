#include "mining_script_override.h"

namespace dinero {

bool g_mining_override_active = false;
int g_mining_override_witver = 0;
std::vector<uint8_t> g_mining_override_witprog;
MiningAddressType g_mining_override_type = MiningAddressType::TRANSPARENT;

}  // namespace dinero
