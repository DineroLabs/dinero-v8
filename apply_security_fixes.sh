#!/bin/bash
# Apply Critical Security Fixes to hd_wallet.cpp

echo "🔧 Applying Security Fixes to hd_wallet.cpp"
echo "==========================================="
echo ""

cd /Users/haydarevich/Documents/DineroCoin

# Backup original
cp src/wallet/hd_wallet.cpp src/wallet/hd_wallet.cpp.backup

echo "✅ Created backup: src/wallet/hd_wallet.cpp.backup"
echo ""

# Create the patched version
cat > src/wallet/hd_wallet_PATCHED.cpp << 'PATCH_EOF'
// Add this to includes section (around line 10):
#include <openssl/crypto.h>  // For OPENSSL_cleanse

// In DeriveNextAddress() function (after line 244):
std::string HDWallet::DeriveNextAddress() {
  // ✅ FIX #1: Bounds check for address index overflow
  if (index_ >= 0x80000000) {
    throw std::runtime_error("Address index overflow: maximum 2^31-1 addresses");
  }
  
  std::string addr = DeriveAddressAt(index_);
  index_++;
  Save();
  return addr;
}

// In DeriveAddressAt() function (before line 298, before context destroy):
  // Compute HASH160(compressed pubkey)
  secp256k1_pubkey P;
  if (!secp256k1_ec_pubkey_create(ctx, &P, k)) throw std::runtime_error("pubkey create failed");
  uint8_t pub[33]; size_t publen=33;
  secp256k1_ec_pubkey_serialize(ctx, pub, &publen, &P, SECP256K1_EC_COMPRESSED);
  uint8_t prog20[20];
  Hash160(pub, publen, prog20);

  // ✅ FIX #2: CRITICAL - Zeroize private key material from stack
  OPENSSL_cleanse(k, sizeof(k));
  OPENSSL_cleanse(I, sizeof(I));
  OPENSSL_cleanse(c, sizeof(c));

  secp256k1_context_destroy(ctx);

// In GetPrivateKeyAt() function (after derive_norm calls, around line 648):
  derive_norm(0);
  
  // ✅ FIX #3: Bounds check
  if (index >= 0x80000000) throw std::runtime_error("Invalid non-hardened index");
  
  derive_norm(index);
  
  // Copy private key BEFORE zeroization
  std::vector<uint8_t> privkey(k, k + 32);
  
  // ✅ FIX #4: Zeroize temporary key material
  OPENSSL_cleanse(k, sizeof(k));
  OPENSSL_cleanse(I, sizeof(I));
  OPENSSL_cleanse(c, sizeof(c));
  
  secp256k1_context_destroy(ctx);
  
  return privkey;
}
PATCH_EOF

echo "📝 Security Fixes to Apply:"
echo ""
echo "1. ✅ Add OPENSSL_cleanse() to DeriveAddressAt()"
echo "   - Zeroize k[32], I[64], c[32] before return"
echo "   - Prevents private key remnants on stack"
echo ""
echo "2. ✅ Add OPENSSL_cleanse() to GetPrivateKeyAt()"
echo "   - Same zeroization after copying key"
echo ""
echo "3. ✅ Add index bounds check in DeriveNextAddress()"
echo "   - Prevent overflow past 2^31-1"
echo ""
echo "4. ✅ Add openssl/crypto.h include"
echo "   - Required for OPENSSL_cleanse()"
echo ""
echo "========================================="
echo ""
echo "🎯 Manual Steps Required:"
echo ""
echo "1. Open: src/wallet/hd_wallet.cpp"
echo ""
echo "2. Add to includes (line ~10):"
echo "   #include <openssl/crypto.h>"
echo ""
echo "3. In DeriveAddressAt() (line ~295, before secp256k1_context_destroy):"
echo "   OPENSSL_cleanse(k, sizeof(k));"
echo "   OPENSSL_cleanse(I, sizeof(I));"
echo "   OPENSSL_cleanse(c, sizeof(c));"
echo ""
echo "4. In DeriveNextAddress() (line ~244, at start):"
echo "   if (index_ >= 0x80000000) {"
echo "     throw std::runtime_error(\"Address index overflow: maximum 2^31-1 addresses\");"
echo "   }"
echo ""
echo "5. In GetPrivateKeyAt() (line ~648, before context destroy):"
echo "   std::vector<uint8_t> privkey(k, k + 32);"
echo "   OPENSSL_cleanse(k, sizeof(k));"
echo "   OPENSSL_cleanse(I, sizeof(I));"
echo "   OPENSSL_cleanse(c, sizeof(c));"
echo "   secp256k1_context_destroy(ctx);"
echo "   return privkey;"
echo ""
echo "========================================="
echo ""
echo "After applying fixes, run:"
echo "  ./test_bip84_security.sh"
echo ""
echo "Then rebuild:"
echo "  cmake --build build -j8"
echo ""

rm src/wallet/hd_wallet_PATCHED.cpp

