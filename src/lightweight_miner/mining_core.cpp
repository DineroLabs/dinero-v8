#include "lightweight_miner/mining_core.h"
#include "common/logger.h"

namespace dinero {

MiningCore::MiningCore() {
    g_logger.info("Initializing lightweight mining core");
}

MiningCore::~MiningCore() {
    g_logger.info("Shutting down lightweight mining core");
}

bool MiningCore::initialize() {
    g_logger.info("Lightweight mining core initialization started");
    return true;
}

void MiningCore::shutdown() {
    g_logger.info("Lightweight mining core shutdown started");
}

} // namespace dinero 