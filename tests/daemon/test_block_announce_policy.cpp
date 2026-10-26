#include "daemon/block_announce_policy.h"

#include <cstdio>
#include <cstdlib>

using dinero::daemon::ShouldAnnounceConnectedBlock;

static int failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
    Check(ShouldAnnounceConnectedBlock(139'700, 139'700), "tip block is announced");
    Check(ShouldAnnounceConnectedBlock(139'694, 139'700), "a block 6 behind is still announced");
    Check(!ShouldAnnounceConnectedBlock(139'693, 139'700), "a block 7 behind is historical");
    Check(!ShouldAnnounceConnectedBlock(128'476, 139'700), "snapshot replay blocks are not announced");
    Check(ShouldAnnounceConnectedBlock(10, std::nullopt), "unknown header height keeps announcing");
    Check(ShouldAnnounceConnectedBlock(139'701, 139'700), "a block above the best header is announced");
    if (failures) return EXIT_FAILURE;
    std::puts("block announce policy: ok");
    return EXIT_SUCCESS;
}
