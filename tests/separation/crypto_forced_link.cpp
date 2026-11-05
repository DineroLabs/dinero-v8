// Crypto forced-link probe (separation Task 1.2).
//
// Linked as: dinero_crypto whole-archive + only its declared link deps.
// Whole-archive pulls in every object, so the link succeeds only if each
// one resolves against what dinero_crypto declares (e.g. src/crypto/bip39.cpp
// needs dinero::crypto::PBKDF2_HMAC_SHA512). main itself references nothing.

#include <cstdio>

int main() {
    std::printf("crypto forced-link OK\n");
    return 0;
}
