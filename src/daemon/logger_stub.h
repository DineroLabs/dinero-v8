#pragma once
#include <iostream>
#include <string>

namespace dinero {
    // Simple logger stub for db_meta_utils
    struct Logger {
        inline void error(const std::string& msg) const {
            std::cerr << "[ERROR] " << msg << std::endl;
        }
    };

    extern Logger g_logger;
}
