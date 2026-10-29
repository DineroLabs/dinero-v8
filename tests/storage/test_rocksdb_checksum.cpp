#include "util/crc32c.h"
#include "util/crc32c_arm64.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#if defined(HAVE_ARM64_CRC)
extern bool pmull_runtime_flag;
#endif

namespace {
uint32_t Reference(uint32_t previous, const uint8_t* bytes, size_t size) {
    uint32_t crc = previous ^ 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    }
    return crc ^ 0xffffffffU;
}
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        std::vector<uint8_t> bytes(8192);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<uint8_t>((i * 193 + i / 7 + 31) & 255);
        std::vector<size_t> lengths;
        for (size_t i = 0; i <= 33; ++i) lengths.push_back(i);
        for (size_t n : {63,64,65,255,256,257,1007,1008,1023,1024,1025,
                         2047,2048,2049,4095,4096,4097}) lengths.push_back(n);
        const std::array<uint32_t,4> seeds{0,1,0xdeadbeefU,0xffffffffU};
        for (size_t offset = 0; offset < 16; ++offset) {
            for (size_t length : lengths) {
                for (uint32_t seed : seeds) {
                    const auto* data = bytes.data() + offset;
                    const auto expected = Reference(seed, data, length);
                    const auto* chars = reinterpret_cast<const char*>(data);
                    Require(rocksdb::crc32c::Extend(seed, chars, length) == expected,
                            "public checksum differs from bitwise reference");
                    const auto split = length / 2;
                    const auto first = rocksdb::crc32c::Extend(seed, chars, split);
                    Require(rocksdb::crc32c::Extend(first, chars + split, length - split) == expected,
                            "incremental checksum differs from reference");
                }
            }
        }
        std::cout << "PASS RocksDBChecksum.ReferenceAndIncremental\n";
#if defined(HAVE_ARM64_CRC)
        Require(crc32c_runtime_check(), "ARM CRC instructions unavailable on qualification host");
        Require(crc32c_pmull_runtime_check(), "ARM PMULL unavailable on qualification host");
        struct RestoreFlag {
            bool previous = pmull_runtime_flag;
            ~RestoreFlag() { pmull_runtime_flag = previous; }
        } restore;
        for (bool parallel : {false,true}) {
            pmull_runtime_flag = parallel;
            for (size_t offset = 0; offset < 16; ++offset) {
                for (size_t length : lengths) {
                    for (uint32_t seed : seeds) {
                        const auto* data = bytes.data() + offset;
                        Require(crc32c_arm64(seed, data, length) == Reference(seed, data, length),
                                "ARM checksum differs from reference");
                    }
                }
            }
            std::cout << (parallel ? "PASS RocksDBChecksum.ArmParallel\n"
                                  : "PASS RocksDBChecksum.ArmSerial\n");
        }
#else
        std::cout << "ARM checksum execution unavailable in this architecture build\n";
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL RocksDBChecksum: " << e.what() << '\n';
        return 1;
    }
}
