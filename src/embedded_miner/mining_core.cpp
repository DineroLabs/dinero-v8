#include "embedded_miner/mining_core.h"
#include "common/logger.h"

namespace dinero {

MiningCore::MiningCore() {
    g_logger.info("Initializing embedded mining core");
}

MiningCore::~MiningCore() {
    g_logger.info("Shutting down embedded mining core");
}

bool MiningCore::initialize() {
    g_logger.info("Embedded mining core initialization started");
    return true;
}

void MiningCore::shutdown() {
    g_logger.info("Embedded mining core shutdown started");
}

} // namespace dinero 