#include "p2p/sha256d.h"
#include "common/sha256d.h"

namespace din::crypto {

std::array<uint8_t,32> sha256_once(const uint8_t* p, size_t n) {
    Dinero::Common::sha256 hasher;
    hasher.update(p, n);
    std::vector<uint8_t> hash = hasher.finalize();
    
    std::array<uint8_t,32> result;
    std::copy(hash.begin(), hash.end(), result.begin());
    return result;
}

std::array<uint8_t,32> sha256d(const uint8_t* p, size_t n) {
    // First SHA256
    auto h1 = sha256_once(p, n);
    
    // Second SHA256 (double hash)
    Dinero::Common::sha256 hasher;
    hasher.update(h1.data(), h1.size());
    std::vector<uint8_t> hash2 = hasher.finalize();
    
    std::array<uint8_t,32> result;
    std::copy(hash2.begin(), hash2.end(), result.begin());
    return result;
}

} // namespace din::crypto
