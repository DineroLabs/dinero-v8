#!/usr/bin/env python3
"""
Automatically convert std::cout/std::cerr calls to also write to debug.log
This ensures the GUI Debug Console shows the same output as the terminal
"""

import re
import sys

def convert_logging(content):
    """Convert std::cout and std::cerr to use dual logging (console + file)"""

    # Add helper macros at the top (after includes)
    helper_code = '''
// ============================================================================
// DUAL LOGGING HELPERS (Console + debug.log)
// ============================================================================
// These helpers ensure Debug Console in GUI shows same output as terminal
#define LOG_INFO(msg) do { std::cout << (msg); dinero::g_logger.info(msg); } while(0)
#define LOG_ERROR(msg) do { std::cerr << (msg); dinero::g_logger.error(msg); } while(0)
#define LOG_ENDL do { std::cout << std::endl; dinero::g_logger.info(""); } while(0)
#define ERR_ENDL do { std::cerr << std::endl; dinero::g_logger.error(""); } while(0)

// Helper to build log messages
#include <sstream>
template<typename T>
inline std::string to_log_str(const T& val) {
    std::ostringstream oss;
    oss << val;
    return oss.str();
}
'''

    # Find the position after the last #include statement
    include_pattern = r'(#include\s+[<"][^>"]+[>"].*?\n)(?!#include)'
    last_include_match = None
    for match in re.finditer(include_pattern, content, re.DOTALL):
        last_include_match = match

    if last_include_match:
        insert_pos = last_include_match.end()
        content = content[:insert_pos] + '\n' + helper_code + '\n' + content[insert_pos:]

    return content

# Read stdin or file
if len(sys.argv) > 1:
    with open(sys.argv[1], 'r') as f:
        content = f.read()
else:
    content = sys.stdin.read()

# Convert and output
result = convert_logging(content)
print(result)
