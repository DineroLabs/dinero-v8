#include "internal_miner/gui_mining.h"
#include "common/logger.h"

namespace dinero {

GuiMining::GuiMining() {
    g_logger.info("Initializing GUI mining component");
}

GuiMining::~GuiMining() {
    g_logger.info("Shutting down GUI mining component");
}

bool GuiMining::initialize() {
    g_logger.info("GUI mining initialization started");
    return true;
}

void GuiMining::shutdown() {
    g_logger.info("GUI mining shutdown started");
}

} // namespace dinero 