// Minimal logger stub for test compilation
#pragma once
#include <string>

namespace dinero {
    class Logger {
    public:
        void info(const std::string&) {}
        void warning(const std::string&) {}
        void warn(const std::string&) {}  // Alias
        void error(const std::string&) {}
    };
    
    inline Logger g_logger;  // Define inline to avoid multiple definition
}
