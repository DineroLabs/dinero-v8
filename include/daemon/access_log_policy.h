#pragma once
#include <atomic>
#include <string>

namespace dinero {

struct AccessLogPolicy {
    std::atomic<uint64_t> ok_counter{0};
    std::atomic<uint64_t> gmi_counter{0};
    uint32_t sample_any = 50;     // 1/50 successes
    uint32_t sample_gmi = 200;    // 1/200 for getmininginfo
    uint32_t slow_ms = 250;       // always log if slower

    bool sample_ok(const std::string& method) {
        if (method == "getmininginfo") {
            return (gmi_counter.fetch_add(1) % sample_gmi) == 0;
        }
        return (ok_counter.fetch_add(1) % sample_any) == 0;
    }
};

extern AccessLogPolicy g_accesslog;

} // namespace dinero
