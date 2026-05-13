// Copyright (c) 2026 Dinero Labs.
//
// Phase 5 Wave 1 — Shielded key-derivation test vectors.
//
// Pins canonical outputs of `DeriveShieldedAccount` against a deterministic
// 64-byte seed so an independent implementation can verify byte parity for
// every component of the Sapling-shape derivation:
//
//   sk → (ask, nsk, ovk, dk) via PRF + DSTs
//   (ask, nsk) → (ak, nk) via secp256k1 scalar mul + BIP340 even-y normalise
//   ivk = Poseidon2(ak, nk)
//
// Diversifier generation and pk_d derivation (Wave 2) are NOT covered here.

#include <gtest/gtest.h>

#include "wallet/shielded_derivation.h"

#include "../external/bech32/bech32.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace dinero::wallet::shielded::testing {
namespace {

using consensus::shielded::Hash;

std::string Hex(const Hash& h) {
    std::string out;
    out.reserve(64);
    for (uint8_t b : h) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

// Canonical 64-byte test seed — first 32 bytes the ASCII string
// "DIN/v7/shielded/derivation/v1\0\0\0", second 32 bytes the same string
// XOR'd with 0xFF, so the seed pins are clearly deterministic and the
// halves differ. Matches the simple "seed_a" pin in the spec §8.
std::array<uint8_t, 64> CanonicalSeed() {
    constexpr const char kTag[] = "DIN/v7/shielded/derivation/v1";
    constexpr std::size_t kTagLen = sizeof(kTag) - 1;  // exclude NUL
    std::array<uint8_t, 64> s{};
    std::memcpy(s.data(), kTag, kTagLen);
    for (std::size_t i = 0; i < 32; ++i) {
        s[32 + i] = static_cast<uint8_t>(s[i] ^ 0xFF);
    }
    return s;
}

TEST(ShieldedDerivation, DstHashCanonicalLayout) {
    Hash h = DstToHash(kDstAsk);
    // 19 ASCII bytes "DIN/v7/shielded/ask" at offsets 0..18, zero pad after.
    // Offsets: 0:D 1:I 2:N 3:/ 4:v 5:7 6:/ 7:s 8:h 9:i 10:e 11:l 12:d
    //          13:e 14:d 15:/ 16:a 17:s 18:k.
    EXPECT_EQ(h[0],  'D');
    EXPECT_EQ(h[3],  '/');
    EXPECT_EQ(h[7],  's');
    EXPECT_EQ(h[14], 'd');  // last char of "shielded"
    EXPECT_EQ(h[15], '/');
    EXPECT_EQ(h[18], 'k');  // last char of "ask"
    for (std::size_t i = 19; i < h.size(); ++i) {
        EXPECT_EQ(h[i], 0u) << "tail byte " << i << " must be zero";
    }
}

TEST(ShieldedDerivation, PRFIsDeterministic) {
    Hash sk{};
    sk[0] = 0xAA;  // arbitrary fixed key
    Hash a = ShieldedPRF(sk, kDstAsk);
    Hash b = ShieldedPRF(sk, kDstAsk);
    EXPECT_EQ(a, b);
    Hash c = ShieldedPRF(sk, kDstNsk);
    EXPECT_NE(a, c) << "different DSTs must produce different PRF outputs";
}

class ShieldedDerivationVectorFixture : public ::testing::Test {
protected:
    ShieldedAccountKeys keys;

    void SetUp() override {
        auto seed = CanonicalSeed();
        keys = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);
    }
};

TEST_F(ShieldedDerivationVectorFixture, AllComponentsAreNonZero) {
    auto non_zero = [](const Hash& h) {
        for (uint8_t b : h) {
            if (b != 0) return true;
        }
        return false;
    };
    EXPECT_TRUE(non_zero(keys.sk));
    EXPECT_TRUE(non_zero(keys.ask));
    EXPECT_TRUE(non_zero(keys.nsk));
    EXPECT_TRUE(non_zero(keys.ovk));
    EXPECT_TRUE(non_zero(keys.dk));
    EXPECT_TRUE(non_zero(keys.ak));
    EXPECT_TRUE(non_zero(keys.nk));
    EXPECT_TRUE(non_zero(keys.ivk));
}

TEST_F(ShieldedDerivationVectorFixture, AllComponentsAreDistinct) {
    EXPECT_NE(keys.ask, keys.nsk);
    EXPECT_NE(keys.ovk, keys.dk);
    EXPECT_NE(keys.ak,  keys.nk);
    EXPECT_NE(keys.ask, keys.ak);   // scalar vs pubkey
    EXPECT_NE(keys.ivk, keys.ovk);  // ivk is Poseidon-derived; ovk is PRF-direct
}

TEST_F(ShieldedDerivationVectorFixture, AccountIsolation) {
    auto seed = CanonicalSeed();
    auto k1 = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/1);
    EXPECT_NE(keys.sk,  k1.sk);
    EXPECT_NE(keys.ask, k1.ask);
    EXPECT_NE(keys.ak,  k1.ak);
    EXPECT_NE(keys.ivk, k1.ivk);
}

TEST_F(ShieldedDerivationVectorFixture, Determinism) {
    auto seed = CanonicalSeed();
    auto k_again = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);
    EXPECT_EQ(keys.sk,  k_again.sk);
    EXPECT_EQ(keys.ask, k_again.ask);
    EXPECT_EQ(keys.ak,  k_again.ak);
    EXPECT_EQ(keys.ivk, k_again.ivk);
}

TEST_F(ShieldedDerivationVectorFixture, PinnedHexVector1Account0) {
    // Seed = CanonicalSeed() (see fixture), account = 0. Captured 2026-04-27.
    EXPECT_EQ(Hex(keys.sk),
              "0afa9463b4d5f06c7d4e9cf14f9d261eaf6c7a0ba243453f5d4308ddc415d9e0");
    EXPECT_EQ(Hex(keys.ask),
              "a03942071a4e3c2b821b4db65f7930255e46484354b656387e9be77e6b695794");
    EXPECT_EQ(Hex(keys.nsk),
              "4d548e2eabaab49cb2e5877bfaad6e456c4033bcc2bfb1882c1f991d1aeb8e3e");
    EXPECT_EQ(Hex(keys.ovk),
              "5eff91d8d132177c83f2302494d879ad01e064846a767617170406d48627ecc9");
    EXPECT_EQ(Hex(keys.dk),
              "7ca608cc6062bfebd3d1a6f7128cdbe9befc60edbb4fc29060fc6219e782c3f2");
    EXPECT_EQ(Hex(keys.ak),
              "864ba7ec6376210f1568f972d907b003723ff985e65305e620effb56789bcdff");
    EXPECT_EQ(Hex(keys.nk),
              "dcfcd14d2739a61201e4d870751c3592bddf8f4b591b9c2abefbf859fd5e19ff");
    EXPECT_EQ(Hex(keys.ivk),
              "51c856061f52ffa07c1f2f05cd0f1b0ded8e94428809044c1c486aba27e492c5");
}

TEST(ShieldedDerivationRejection, RejectsBadSeedLen) {
    std::array<uint8_t, 32> short_seed{};
    EXPECT_THROW(DeriveShieldedAccount(short_seed.data(), short_seed.size(), 0),
                 std::runtime_error);
    EXPECT_THROW(DeriveShieldedAccount(nullptr, 64, 0), std::runtime_error);
}

// ── Phase 5 Wave 2 vectors ───────────────────────────────────────────

std::string DiversifierHex(const Diversifier& d) {
    std::string s;
    s.reserve(22);
    for (uint8_t b : d) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

TEST_F(ShieldedDerivationVectorFixture, DiversifierIsDeterministic) {
    auto a = ChaCha20Diversifier(keys.dk, /*j=*/0);
    auto b = ChaCha20Diversifier(keys.dk, /*j=*/0);
    EXPECT_EQ(a, b);
    auto c = ChaCha20Diversifier(keys.dk, /*j=*/1);
    EXPECT_NE(a, c) << "different j must yield different diversifier";
}

TEST_F(ShieldedDerivationVectorFixture, HashToPointIsDeterministic) {
    Diversifier d{};
    d[0] = 0x01;  // arbitrary fixed
    Hash a = HashToPoint(d, kDstDiv);
    Hash b = HashToPoint(d, kDstDiv);
    EXPECT_EQ(a, b);
    Diversifier d2 = d;
    d2[10] = 0xFF;
    Hash c = HashToPoint(d2, kDstDiv);
    EXPECT_NE(a, c);
}

TEST_F(ShieldedDerivationVectorFixture, PkDDeterministicAndDistinctPerJ) {
    auto addr0 = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    auto addr1 = DeriveDiversifiedAddress(keys, /*j=*/1, kHrpRegtest);
    EXPECT_NE(addr0.d,    addr1.d);
    EXPECT_NE(addr0.pk_d, addr1.pk_d);
    EXPECT_NE(addr0.address, addr1.address);
    auto addr0_again = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    EXPECT_EQ(addr0.address, addr0_again.address);
}

TEST_F(ShieldedDerivationVectorFixture, AddressHrpsMatch) {
    auto a_main = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpMainnet);
    auto a_test = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpTestnet);
    auto a_reg  = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    EXPECT_EQ(a_main.d,    a_test.d);
    EXPECT_EQ(a_main.pk_d, a_test.pk_d);
    EXPECT_EQ(a_main.payload, a_reg.payload);
    EXPECT_NE(a_main.address, a_test.address);  // HRP differs
    EXPECT_TRUE(a_main.address.rfind("dins1",  0) == 0);
    EXPECT_TRUE(a_test.address.rfind("tdins1", 0) == 0);
    EXPECT_TRUE(a_reg.address.rfind("rdins1",  0) == 0);
}

TEST_F(ShieldedDerivationVectorFixture, PinnedHexVector1Account0Wave2J0) {
    auto v = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpMainnet);
    EXPECT_EQ(DiversifierHex(v.d),
              "6b92d6a2de35177cada44c");
    EXPECT_EQ(Hex(v.pk_d),
              "981db4b85ce150d7e74768cd6d9147148cba846857289d5c585b0681f9a469f9");
    EXPECT_EQ(v.address,
              "dins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535ljkd4ggr");
    auto t = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpTestnet);
    EXPECT_EQ(t.address,
              "tdins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535lj5qhwc6");
    auto r = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    EXPECT_EQ(r.address,
              "rdins1dwfddgk7x5thetdyfjvpmd9ctns4p4l8ga5v6mv3gu2gew5ydptj382utpdsdq0e535lj0pxq49");
}

// ── Phase 5 Wave 3: address decoding + ECDH/AEAD ─────────────────────

TEST_F(ShieldedDerivationVectorFixture, DecodeAddressRoundTripsForAllHrps) {
    for (const std::string hrp : {std::string(kHrpMainnet),
                                  std::string(kHrpTestnet),
                                  std::string(kHrpRegtest)}) {
        auto enc = DeriveDiversifiedAddress(keys, /*j=*/0, hrp);
        auto dec = DecodeShieldedAddress(enc.address);
        EXPECT_EQ(dec.hrp,     hrp);
        EXPECT_EQ(dec.d,       enc.d);
        EXPECT_EQ(dec.pk_d,    enc.pk_d);
        EXPECT_EQ(dec.payload, enc.payload);
    }
}

TEST(ShieldedDerivationDecode, RejectsNonShieldedHrps) {
    EXPECT_THROW(DecodeShieldedAddress(
        "din1pqyqsywdkqz4dz9hfff9zfwryjs4khufprr2elx2qedlcz4cyk67r3qq2yzg2v"),
        std::runtime_error);  // taproot din1p
    EXPECT_THROW(DecodeShieldedAddress(
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"),
        std::runtime_error);  // bitcoin bc1
    EXPECT_THROW(DecodeShieldedAddress("notabech32mstring"),
                 std::runtime_error);
    EXPECT_THROW(DecodeShieldedAddress(""),
                 std::runtime_error);
}

// ── §7.1 enforcement: HRP-validated address parsing for shielded RPCs.
// Pins the exact reject set so any new wallet/RPC entry point that
// constructs shielded outputs MUST reject these strings before doing
// any cryptographic work. wallet.transfer's `address` param already
// routes through DecodeShieldedAddress; this test fixes the contract.
TEST(ShieldedDerivationSpec71Enforcement, RejectsAllNonShieldedHrps) {
    // Realistic addresses with non-shielded HRPs, all bech32 or
    // bech32m, all should fail the shielded parse:
    const std::vector<std::string> non_shielded = {
        // Dinero Taproot (din1p…)
        "din1pqyqsywdkqz4dz9hfff9zfwryjs4khufprr2elx2qedlcz4cyk67r3qq2yzg2v",
        // Dinero P2MR ML-DSA (din1r…) — same HRP family but different
        // length / payload shape; the shielded decoder rejects on HRP.
        "din1rqyqsywdkqz4dz9hfff9zfwryjs4khufprr2elx2qedlcz4cyk67r3qq2yzg2v",
        // Bitcoin segwit/taproot (bc1…)
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
        "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
        // Bitcoin testnet (tb1…)
        "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx",
        // Litecoin (ltc1…)
        "ltc1qw508d6qejxtdg4y5r3zarvary0c5xw7kgmn4n9",
        // Zcash sapling (zs…)
        "zs1z7rejlpsa98s2rrrfkwmaxu53e4ue0ulcrw0h4x5g8jl04tak0d3mm47vdtahatqrlkngh9sly",
        // Random non-bech32 garbage
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",
        // Empty + obvious junk
        "",
        "notabech32address",
        "rdins1tooshort",
    };
    for (const auto& addr : non_shielded) {
        EXPECT_THROW(DecodeShieldedAddress(addr), std::runtime_error)
            << "shielded decoder must reject: '" << addr << "'";
    }
}

TEST(ShieldedDerivationDecode, RejectsBech32NotBech32m) {
    // Construct the same payload via bech32 (BIP173) checksum and confirm
    // we reject it (post-BIP350 shielded addresses MUST be bech32m).
    AddressPayload payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    std::vector<uint8_t> data5;
    bech32::convertbits(data5, bytes, 8, 5, /*pad=*/true);
    std::string bech32_addr =
        bech32::EncodeRaw("dins", data5, bech32::Encoding::BECH32);
    ASSERT_FALSE(bech32_addr.empty());
    EXPECT_THROW(DecodeShieldedAddress(bech32_addr), std::runtime_error);
}

TEST(ShieldedDerivationAead, NoteRoundTripIsoMorphic) {
    NotePlaintext note;
    for (std::size_t i = 0; i < note.d.size(); ++i) {
        note.d[i] = static_cast<uint8_t>(i + 1);
    }
    note.value_una = 100'000'000;
    for (std::size_t i = 0; i < note.rcm.size(); ++i) {
        note.rcm[i] = static_cast<uint8_t>(0xA0 + i);
    }
    const std::string memo_text = "hello dinero shielded";
    std::memcpy(note.memo.data(), memo_text.data(), memo_text.size());

    auto bytes = note.Serialize();
    auto round = NotePlaintext::Deserialize(bytes);
    EXPECT_EQ(round.d, note.d);
    EXPECT_EQ(round.value_una, note.value_una);
    EXPECT_EQ(round.rcm, note.rcm);
    EXPECT_EQ(round.memo, note.memo);
}

TEST_F(ShieldedDerivationVectorFixture, EncryptedNoteRoundTrip) {
    // Build a legitimate (d, pk_d) for the wallet's own ivk so we can
    // both encrypt-to-self and decrypt-as-self.
    auto addr = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);

    NotePlaintext note;
    note.d = addr.d;
    note.value_una = 42'000'000;
    note.rcm[0] = 0xCA;
    note.rcm[31] = 0xFE;

    // Deterministic esk for reproducibility.
    Hash esk{};
    esk[0] = 0xE5;
    esk[31] = 0xC0;

    auto encrypted = EncryptNoteForRecipient(addr.d, addr.pk_d, note, &esk);
    EXPECT_EQ(encrypted.size(), kEncryptedNoteBytes);

    auto decrypted = TryDecryptNoteForViewer(keys.ivk, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(decrypted->d,         note.d);
    EXPECT_EQ(decrypted->value_una, note.value_una);
    EXPECT_EQ(decrypted->rcm,       note.rcm);
}

TEST_F(ShieldedDerivationVectorFixture, WrongIvkFailsDecryption) {
    auto addr = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    NotePlaintext note;
    note.d = addr.d;
    note.value_una = 1'000'000;
    Hash esk{};
    esk[0] = 0xE5;
    esk[31] = 0xC0;
    auto encrypted = EncryptNoteForRecipient(addr.d, addr.pk_d, note, &esk);

    // Different account → different ivk.
    auto seed = CanonicalSeed();
    auto other = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/1);
    auto decrypted = TryDecryptNoteForViewer(other.ivk, encrypted);
    EXPECT_FALSE(decrypted.has_value()) << "wrong ivk must NOT decrypt";
}

TEST_F(ShieldedDerivationVectorFixture, TamperedCiphertextFailsDecryption) {
    auto addr = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);
    NotePlaintext note;
    note.d = addr.d;
    note.value_una = 1'000'000;
    Hash esk{};
    esk[0] = 0xE5;
    esk[31] = 0xC0;
    auto encrypted = EncryptNoteForRecipient(addr.d, addr.pk_d, note, &esk);

    // Flip a byte in the ciphertext region (after epk).
    encrypted[100] ^= 0x01;
    auto decrypted = TryDecryptNoteForViewer(keys.ivk, encrypted);
    EXPECT_FALSE(decrypted.has_value()) << "AEAD tag mismatch must reject";
}

// ── Phase 5 Wave 3b: note spend-key derivation + A→B round trip ─────

TEST(ShieldedDerivationSpendKey, IsDeterministicAndDistinct) {
    Hash rcm_a{};
    rcm_a[0]  = 0x11;
    rcm_a[31] = 0x22;
    Hash rcm_b{};
    rcm_b[0]  = 0x33;
    rcm_b[31] = 0x44;
    EXPECT_EQ(DeriveNoteSpendKey(rcm_a), DeriveNoteSpendKey(rcm_a));
    EXPECT_NE(DeriveNoteSpendKey(rcm_a), DeriveNoteSpendKey(rcm_b));
    EXPECT_NE(DeriveNoteSpendKey(rcm_a), Hash{}) << "must not be zero";
}

// Vector 2 — encrypted note round-trip with deterministic inputs.
// Pins the canonical bytes that an independent implementation must
// reproduce given the same (recipient_d, recipient_pk_d, esk, rcm,
// value, memo). Documents the full ECDH/AEAD wire output: the 32-byte
// epk and the 32+579-byte (epk||ct||tag) container.
TEST_F(ShieldedDerivationVectorFixture, PinnedHexVector2EncryptedNote) {
    auto addr = DeriveDiversifiedAddress(keys, /*j=*/0, kHrpRegtest);

    NotePlaintext note;
    note.d         = addr.d;
    note.value_una = 100'000'000;  // 1 DIN
    // rcm: first 24 bytes ASCII "DIN/v7/shielded/note/rcm", zero pad.
    constexpr const char kRcmTag[] = "DIN/v7/shielded/note/rcm";
    std::memcpy(note.rcm.data(), kRcmTag, sizeof(kRcmTag) - 1);
    // memo: first 21 bytes ASCII "DIN test note plain v1", zero pad.
    constexpr const char kMemoTag[] = "DIN test note plain v1";
    std::memcpy(note.memo.data(), kMemoTag, sizeof(kMemoTag) - 1);

    // esk: first 24 bytes ASCII "DIN/v7/shielded/note/esk", zero pad.
    Hash esk{};
    constexpr const char kEskTag[] = "DIN/v7/shielded/note/esk";
    std::memcpy(esk.data(), kEskTag, sizeof(kEskTag) - 1);

    auto encrypted = EncryptNoteForRecipient(addr.d, addr.pk_d, note, &esk);

    // Sanity: round-trip decrypts to identical plaintext.
    auto decrypted = TryDecryptNoteForViewer(keys.ivk, encrypted);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_EQ(decrypted->d,         note.d);
    EXPECT_EQ(decrypted->value_una, note.value_una);
    EXPECT_EQ(decrypted->rcm,       note.rcm);

    auto bytes_to_hex = [](const uint8_t* p, std::size_t n) {
        std::string s;
        s.reserve(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", p[i]);
            s += buf;
        }
        return s;
    };

    // epk = first 32 bytes of the encrypted_note container.
    EXPECT_EQ(bytes_to_hex(encrypted.data(), 32),
              "94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f");
    // Full 611-byte encrypted_note container (epk || ct || tag).
    EXPECT_EQ(bytes_to_hex(encrypted.data(), encrypted.size()),
              "94d6195348f85b3dfca0caeb9f0a3a398345542793088f5b960ba3a134494c7f"
              "bf48bb6d359922610ff31ceadd6872092a20e35c9aa1e82e666ef880af7450e8"
              "26e2385e8685a7638ddaa5ee0ccdce6414b825b9233dacb3bb87cd3463b4e40e"
              "246772fd6e2a9bab03834141fbca84782a3476ffb4b708d3d32b0e18b4094eb5"
              "7b473fd7b401ef1dd4af9c35c14426b7006fb6a29db599ce12594458dd233b30"
              "157988ea3d95499d4a838143ee43d22f0af54173735c919eeaed19a320c72d13"
              "22caba24b161f872851ea6db9e5e46b23fde0ccde181ff31863dbd04bbe72c67"
              "a3292b55934d48db77137afe3cdf5db3bc7ea9705939f1163b2920a2d3a917a0"
              "b87364b68ffcaaeda17a0b34fe6e15637a457029b54c7f0a4ea2f2d019d217c1"
              "384f74d76e86cfd903a7878256b1d01c20cc4be94354a4d6d2bc2c361491492c"
              "64e6b047e92587e3b4ada8aedfcc88e87e70b1d7d35b2c1c0a2539384450e04a"
              "ac15eebf7a6f3f7606bb1e74a479eabe17696db5af14bc5c775eb71a14e13ea7"
              "7bc914815e04368ce7cee67acc27e40131b2bd002dfb99880797914547b0b92b"
              "a88cb19b68b465514cdecab53de8e15e9b9bbe6877fc1c8f5d629d99f553acab"
              "2948febe585a8163ec05a9a2b2d46d2f90bdb802985577248abc5560354aadea"
              "e52a7ad584eb15e4b5329306205b213fd9c366f69cc7380716dcffadf7677cf6"
              "ca12dcab99be4ab07b7e4284031a4e639c4ceb9e092e8bec187c6ff3b37828c6"
              "e4a0bf59761d8ef12d79279bfff1be6c1db607f34524a9ad63291be18604c802"
              "7524a02de17fdfecbcedcfbab2e83e6028c8a4d9be01b2f8b9dc98b92cbd2d8c"
              "14b14a");
}

// End-to-end vector: account A creates a note for account B's diversified
// address. B detects the note via TryDecryptNote, recovers rcm, derives
// the per-note spend key, and reconstructs the on-chain commitment. This
// is the consensus-relevant half of any-recipient shielded transfer:
// proves the receiver can take a transparent address-style shielded
// payment and produce a SpendWitness that the existing circuit accepts.
TEST(ShieldedDerivationAddressedTransfer, AccountAtoAccountBRoundTrip) {
    using ::dinero::consensus::shielded::NoteCommitment;
    using ::dinero::consensus::shielded::PoseidonHash2;

    // ── Set up two distinct accounts under the canonical seed.
    auto seed = []() {
        constexpr const char kTag[] = "DIN/v7/shielded/derivation/v1";
        constexpr std::size_t kTagLen = sizeof(kTag) - 1;
        std::array<uint8_t, 64> s{};
        std::memcpy(s.data(), kTag, kTagLen);
        for (std::size_t i = 0; i < 32; ++i) {
            s[32 + i] = static_cast<uint8_t>(s[i] ^ 0xFF);
        }
        return s;
    }();
    auto keys_a = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/0);
    auto keys_b = DeriveShieldedAccount(seed.data(), seed.size(), /*account=*/1);
    auto b_addr = DeriveDiversifiedAddress(keys_b, /*j=*/0, kHrpRegtest);

    // ── A: build a note for B at j=0. rcm is fresh randomness chosen
    //      by A; the per-note spend secret is deterministically derived
    //      from rcm so B (decrypting the note) can reconstruct it.
    Hash rcm{};
    rcm[0]  = 0xAB;
    rcm[15] = 0xCD;
    rcm[31] = 0xEF;

    // The on-chain `addr_bind` formula in commitment_tree.cpp takes a
    // 32-byte Hash for `d`. Encode the 11-byte Diversifier into the
    // first 11 bytes of a 32-byte buffer (zero pad after) — matches
    // the AddrBindTag layout convention.
    auto pack_d = [](const Diversifier& d) {
        Hash h{};
        std::memcpy(h.data(), d.data(), d.size());
        return h;
    };

    Hash sk_note     = DeriveNoteSpendKey(rcm);
    Hash pk_note     = PoseidonHash2(sk_note, Hash{});
    Hash value_h     = [&]() {
        Hash h{};
        const uint64_t v = 100'000'000;
        for (int i = 0; i < 8; ++i) {
            h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }
        return h;
    }();
    Hash d_packed    = pack_d(b_addr.d);
    Hash on_chain_commitment =
        NoteCommitment(d_packed, pk_note, value_h, rcm);

    // A encrypts the note plaintext (containing rcm) to B's pk_d.
    NotePlaintext note;
    note.d         = b_addr.d;
    note.value_una = 100'000'000;
    note.rcm       = rcm;
    Hash esk{};
    esk[0]  = 0xE5;
    esk[31] = 0xC0;
    auto encrypted = EncryptNoteForRecipient(b_addr.d, b_addr.pk_d, note, &esk);

    // ── B: detect via ivk (B does not see rcm beforehand).
    auto decrypted = TryDecryptNoteForViewer(keys_b.ivk, encrypted);
    ASSERT_TRUE(decrypted.has_value()) << "B must detect A's payment";
    EXPECT_EQ(decrypted->d,         b_addr.d);
    EXPECT_EQ(decrypted->value_una, 100'000'000u);
    EXPECT_EQ(decrypted->rcm,       rcm);

    // B re-derives the per-note spend key + commitment and verifies it
    // matches the on-chain commitment. After this check passes, B can
    // construct a SpendWitness with sk_note_b == sk_note (sender's) and
    // produce a Spartan spend proof.
    Hash sk_note_b   = DeriveNoteSpendKey(decrypted->rcm);
    Hash pk_note_b   = PoseidonHash2(sk_note_b, Hash{});
    Hash value_h_b   = [&]() {
        Hash h{};
        const uint64_t v = decrypted->value_una;
        for (int i = 0; i < 8; ++i) {
            h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }
        return h;
    }();
    Hash d_packed_b  = pack_d(decrypted->d);
    Hash recomputed_commitment =
        NoteCommitment(d_packed_b, pk_note_b, value_h_b, decrypted->rcm);
    EXPECT_EQ(recomputed_commitment, on_chain_commitment)
        << "receiver's recomputed commitment must match what A put on chain";
    EXPECT_EQ(sk_note_b, sk_note)
        << "deterministic sk derivation must agree across sender/receiver";

    // The wrong account's ivk must NOT decrypt.
    EXPECT_FALSE(TryDecryptNoteForViewer(keys_a.ivk, encrypted).has_value());
}

}  // namespace
}  // namespace dinero::wallet::shielded::testing
