#include <openssl/sha.h>
#include <cstdint>
#include <cstddef>

void sha256(const unsigned char* data, size_t len, unsigned char* hash) {
    SHA256(data, len, hash);
}
