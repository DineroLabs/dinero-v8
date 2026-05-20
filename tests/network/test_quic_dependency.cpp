#include <ngtcp2/ngtcp2.h>

#include <cstring>
#include <iostream>

int main() {
    const ngtcp2_info* info = ngtcp2_version(0);
    if (info == nullptr) {
        std::cerr << "ngtcp2_version returned null\n";
        return 1;
    }
    if (info->version_num != NGTCP2_VERSION_NUM) {
        std::cerr << "ngtcp2 runtime/header version mismatch: runtime="
                  << info->version_num << " header=" << NGTCP2_VERSION_NUM << "\n";
        return 1;
    }
    if (std::strcmp(info->version_str, NGTCP2_VERSION) != 0) {
        std::cerr << "ngtcp2 runtime/header version string mismatch: runtime="
                  << info->version_str << " header=" << NGTCP2_VERSION << "\n";
        return 1;
    }
    std::cout << "ngtcp2 dependency OK: " << info->version_str << "\n";
    return 0;
}
