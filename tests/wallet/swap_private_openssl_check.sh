#!/usr/bin/env bash
# A Linux program that links libcurl must not export the OpenSSL it carries
# (cmake/BuildOptions.cmake, dinero_keep_static_openssl_private): if it does,
# the system's libssl binds to that copy and the first HTTPS certificate check
# crashes. Skipped where the problem cannot arise (not ELF).
#   usage: swap_private_openssl_check.sh <program>
set -euo pipefail
BIN=${1:?program}
[[ "$(uname -s)" == Linux ]] || { echo "SKIP: only ELF programs export symbols this way"; exit 77; }
command -v nm >/dev/null || { echo "SKIP: nm not installed"; exit 77; }
# The program must really carry OpenSSL, or the check below proves nothing.
CARRIED=$(nm --defined-only "$BIN" 2>/dev/null | grep -cE " [TtDdBbRr] (X509_verify_cert|EVP_DigestInit_ex)$" || true)
if [[ "$CARRIED" -eq 0 ]]; then echo "SKIP: $BIN has no static OpenSSL (or is stripped)"; exit 77; fi
EXPORTED=$(nm -D --defined-only "$BIN" | grep -cE " [TDBRW] (X509_|EVP_|SSL_|OPENSSL_|CRYPTO_|ossl_)" || true)
if [[ "$EXPORTED" -ne 0 ]]; then
  echo "FAIL: $BIN exports $EXPORTED OpenSSL symbols; a shared libssl would bind to them:"
  nm -D --defined-only "$BIN" | grep -E " [TDBRW] (X509_|EVP_|SSL_)" | head -5
  exit 1
fi
echo "OK: $BIN carries OpenSSL and exports none of it"
