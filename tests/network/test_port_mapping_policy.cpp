#include "network/port_mapper.h"

#include <chrono>
#include <cstdio>

static int g_fails = 0;

static void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++g_fails;
}

int main() {
    using dinero::network::PortMappingNextActionDelay;
    using namespace std::chrono_literals;

    check(PortMappingNextActionDelay(true, 7200, 300) == 3600s,
          "successful mapping renews at half-life");
    check(PortMappingNextActionDelay(true, 40, 300) == 30s,
          "short leases retain a bounded renewal floor");
    check(PortMappingNextActionDelay(false, 7200, 300) == 300s,
          "failed discovery retries on the configured cadence");
    check(PortMappingNextActionDelay(false, 7200, 1) == 30s,
          "failed discovery cannot busy-loop");

    const auto compile_info = dinero::network::GetPortMappingCompileInfo();
    check(compile_info.available() == (compile_info.upnp || compile_info.natpmp),
          "compile capability reports its concrete backends consistently");

    if (g_fails != 0) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nALL PORT MAPPING POLICY CHECKS PASSED\n");
    return 0;
}
