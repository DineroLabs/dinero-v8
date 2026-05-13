#pragma once

#include <iostream>
#include <string>

namespace dinero {

// Simple logger implementation for testing
class Logger {
public:
    template<typename... Args>
    void info(const std::string& format, Args... args) {
        std::cout << "[INFO] " << format << std::endl;
    }
    
    template<typename... Args>
    void warning(const std::string& format, Args... args) {
        std::cout << "[WARNING] " << format << std::endl;
    }
    
    template<typename... Args>
    void error(const std::string& format, Args... args) {
        std::cerr << "[ERROR] " << format << std::endl;
    }
    
    template<typename... Args>
    void debug(const std::string& format, Args... args) {
        std::cout << "[DEBUG] " << format << std::endl;
    }
};

// Global logger instance
extern Logger g_logger;

} // namespace dinero
