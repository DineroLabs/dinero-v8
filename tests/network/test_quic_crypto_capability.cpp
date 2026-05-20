#include <ngtcp2/ngtcp2.h>

#include <openssl/opensslv.h>
#include <openssl/ssl.h>

#include <iostream>

#if defined(DINERO_NGTCP2_CRYPTO_BACKEND_OSSL)
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#elif defined(DINERO_NGTCP2_CRYPTO_BACKEND_QUICTLS)
#include <ngtcp2/ngtcp2_crypto_quictls.h>
#endif

int main() {
    std::cout << "ngtcp2 core: " << ngtcp2_version(0)->version_str << "\n";
    std::cout << "OpenSSL: " << OpenSSL_version(OPENSSL_VERSION) << "\n";

#if defined(DINERO_NGTCP2_CRYPTO_BACKEND_OSSL)
    if (ngtcp2_crypto_ossl_init() != 0) {
        std::cerr << "ngtcp2 ossl crypto init failed\n";
        return 1;
    }
    std::cout << "ngtcp2 crypto bridge: ossl\n";
    return 0;
#elif defined(DINERO_NGTCP2_CRYPTO_BACKEND_QUICTLS)
    if (ngtcp2_crypto_quictls_init() != 0) {
        std::cerr << "ngtcp2 quictls crypto init failed\n";
        return 1;
    }
    std::cout << "ngtcp2 crypto bridge: quictls\n";
    return 0;
#else
    std::cout
        << "ngtcp2 crypto bridge: unavailable with this OpenSSL; "
        << "encrypted QUIC transport remains gated\n";
    return 0;
#endif
}
