// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_tls_keypair.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>

namespace dinero::network {
namespace {

struct EvpPkeyDeleter { void operator()(EVP_PKEY* p) const { if (p) EVP_PKEY_free(p); } };
struct EvpCtxDeleter  { void operator()(EVP_PKEY_CTX* p) const { if (p) EVP_PKEY_CTX_free(p); } };
struct X509Deleter    { void operator()(X509* p) const { if (p) X509_free(p); } };
struct BioDeleter     { void operator()(BIO* p) const { if (p) BIO_free(p); } };
struct BnDeleter      { void operator()(BIGNUM* p) const { if (p) BN_free(p); } };

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using EvpCtxPtr  = std::unique_ptr<EVP_PKEY_CTX, EvpCtxDeleter>;
using X509Ptr    = std::unique_ptr<X509, X509Deleter>;
using BioPtr     = std::unique_ptr<BIO, BioDeleter>;
using BnPtr      = std::unique_ptr<BIGNUM, BnDeleter>;

bool SetErr(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

EvpPkeyPtr GenerateP256Key(std::string* err) {
    EvpCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    if (!ctx) { SetErr(err, "EVP_PKEY_CTX_new_id failed"); return nullptr; }
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
        SetErr(err, "EVP_PKEY_keygen_init failed"); return nullptr;
    }
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0) {
        SetErr(err, "EVP_PKEY_CTX_set_ec_paramgen_curve_nid failed"); return nullptr;
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) {
        SetErr(err, "EVP_PKEY_keygen failed"); return nullptr;
    }
    return EvpPkeyPtr(raw);
}

bool SetRandomSerial(X509* x509) {
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) return false;
    // Clear top bit so the serial is positive in ASN.1 two's complement.
    buf[0] &= 0x7f;
    BnPtr bn(BN_bin2bn(buf, sizeof(buf), nullptr));
    if (!bn) return false;
    ASN1_INTEGER* serial = ASN1_INTEGER_new();
    if (!serial) return false;
    if (!BN_to_ASN1_INTEGER(bn.get(), serial)) {
        ASN1_INTEGER_free(serial);
        return false;
    }
    const int ok = X509_set_serialNumber(x509, serial);
    ASN1_INTEGER_free(serial);
    return ok == 1;
}

bool AddTextExtension(X509* x509, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) return false;
    const int ok = X509_add_ext(x509, ext, -1);
    X509_EXTENSION_free(ext);
    return ok == 1;
}

}  // namespace

bool GenerateRelayTlsKeypair(std::string* cert_pem,
                             std::string* private_key_pem,
                             std::string* err) {
    if (!cert_pem || !private_key_pem) {
        return SetErr(err, "null output parameter");
    }

    EvpPkeyPtr pkey = GenerateP256Key(err);
    if (!pkey) return false;

    X509Ptr x509(X509_new());
    if (!x509) return SetErr(err, "X509_new failed");

    if (X509_set_version(x509.get(), 2 /* v3 */) != 1) {
        return SetErr(err, "X509_set_version failed");
    }
    if (!SetRandomSerial(x509.get())) {
        return SetErr(err, "serial number generation failed");
    }
    // Valid for ~1 year, with a 60-second backdate so clock skew across the
    // relay path doesn't reject the cert during early use.
    if (!X509_gmtime_adj(X509_get_notBefore(x509.get()), -60)) {
        return SetErr(err, "notBefore set failed");
    }
    if (!X509_gmtime_adj(X509_get_notAfter(x509.get()), 60L * 60 * 24 * 365)) {
        return SetErr(err, "notAfter set failed");
    }
    if (X509_set_pubkey(x509.get(), pkey.get()) != 1) {
        return SetErr(err, "X509_set_pubkey failed");
    }

    X509_NAME* name = X509_get_subject_name(x509.get());
    const auto* cn = reinterpret_cast<const unsigned char*>("localhost");
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, cn, -1, -1, 0) != 1) {
        return SetErr(err, "X509_NAME_add_entry_by_txt failed");
    }
    if (X509_set_issuer_name(x509.get(), name) != 1) {
        return SetErr(err, "X509_set_issuer_name failed");
    }

    // Minimal v3 extensions — SAN=localhost is required by some TLS stacks
    // even when verify_peer=false, and the basicConstraints/keyUsage entries
    // keep ngtcp2/quictls happy with a self-signed leaf cert.
    if (!AddTextExtension(x509.get(), NID_basic_constraints, "critical,CA:FALSE")) {
        return SetErr(err, "basicConstraints extension failed");
    }
    if (!AddTextExtension(x509.get(), NID_key_usage,
                          "critical,digitalSignature,keyEncipherment")) {
        return SetErr(err, "keyUsage extension failed");
    }
    if (!AddTextExtension(x509.get(), NID_ext_key_usage, "serverAuth,clientAuth")) {
        return SetErr(err, "extKeyUsage extension failed");
    }
    if (!AddTextExtension(x509.get(), NID_subject_alt_name, "DNS:localhost")) {
        return SetErr(err, "subjectAltName extension failed");
    }

    if (X509_sign(x509.get(), pkey.get(), EVP_sha256()) == 0) {
        return SetErr(err, "X509_sign failed");
    }

    BioPtr cert_bio(BIO_new(BIO_s_mem()));
    if (!cert_bio || !PEM_write_bio_X509(cert_bio.get(), x509.get())) {
        return SetErr(err, "cert PEM serialize failed");
    }
    BioPtr key_bio(BIO_new(BIO_s_mem()));
    if (!key_bio || !PEM_write_bio_PrivateKey(key_bio.get(), pkey.get(),
                                              nullptr, nullptr, 0, nullptr, nullptr)) {
        return SetErr(err, "key PEM serialize failed");
    }

    char* cert_data = nullptr;
    const long cert_len = BIO_get_mem_data(cert_bio.get(), &cert_data);
    if (cert_len <= 0 || !cert_data) return SetErr(err, "cert PEM read failed");
    char* key_data = nullptr;
    const long key_len = BIO_get_mem_data(key_bio.get(), &key_data);
    if (key_len <= 0 || !key_data) return SetErr(err, "key PEM read failed");

    cert_pem->assign(cert_data, static_cast<size_t>(cert_len));
    private_key_pem->assign(key_data, static_cast<size_t>(key_len));
    return true;
}

}  // namespace dinero::network
