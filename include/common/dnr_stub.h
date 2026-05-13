#pragma once
#include <cstdlib>
#include <cstdio>
#include <string>

namespace din {

inline void stub_hit(const char* file, int line, const char* func, const char* msg) {
    std::fprintf(stderr, "[STUB] %s:%d %s -- %s\n", file, line, func, msg ? msg : "");
    const char* fatal = std::getenv("DIN_FAIL_ON_STUB");
    if (fatal && std::string(fatal) == "1") {
        std::abort();
    }
}

} // namespace din

#define DIN_STUB(MSG) ::din::stub_hit(__FILE__, __LINE__, __func__, (MSG))
#define DIN_UNREACHABLE(MSG) do { ::din::stub_hit(__FILE__, __LINE__, __func__, (MSG)); std::abort(); } while(0)
