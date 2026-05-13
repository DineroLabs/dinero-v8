#include "daemon/config.h"

NodeConfig& GetConfig() {
    static NodeConfig cfg;
    return cfg;
}
