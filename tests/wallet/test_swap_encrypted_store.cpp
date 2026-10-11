// Encrypted swap store: the session (with Alice's secret) never reaches disk
// in the clear, a wrong key or a flipped byte is refused, and every save uses
// a fresh nonce.
#include "wallet/swap/encrypted_store.h"

#include "crypto/sha256.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

using namespace dinero;
using namespace dinero::swap;

constexpr uint32_t kNow = 1'800'000'000;
const Bytes32 kSecret = [] { Bytes32 s{}; s.fill(0x5a); return s; }();

Bytes32 Scalar(uint8_t s) { Bytes32 a{}; a.back() = s; return a; }

SwapSession AliceSession() {
    SwapSession s;
    auto& r = s.record;
    r.role = Role::DinSeller;
    auto& o = r.offer;
    o.network = SwapNetwork::Regtest;
    o.din_amount_una = 10 * 100'000'000ULL;
    o.btc_amount_sat = 1'000'000;
    crypto::CSHA256().Write(kSecret.data(), kSecret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = detail::XOnlyOf(Scalar(3));
    o.btc_claim_pubkey = detail::CompressedOf(Scalar(4));
    o.expires_unix = kNow;
    o.t_btc_unix = kNow + 48 * 3600;
    o.t_din_unix = kNow + 96 * 3600;
    o.n_din_confirmations = 30;
    o.n_btc_confirmations = 1;
    r.accept.offer_id = OfferId(o);
    r.accept.din_claim_pubkey = detail::XOnlyOf(Scalar(5));
    r.accept.btc_refund_pubkey = detail::CompressedOf(Scalar(6));
    r.secret = kSecret;
    s.btc_scan_from_height = 7;
    s.din_payout_script = std::vector<uint8_t>(34, 0x51);
    s.btc_payout_script = std::vector<uint8_t>(34, 0x52);
    return s;
}

std::string Slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream b;
    b << in.rdbuf();
    return b.str();
}

struct TempFile {
    std::string path = (std::filesystem::temp_directory_path() / "swap_encrypted_store_test.swap").string();
    TempFile() { std::remove(path.c_str()); }
    ~TempFile() { std::remove(path.c_str()); }
};

Bytes32 MasterKey(uint8_t b) { Bytes32 k{}; k.fill(b); return k; }

TEST(SwapEncryptedStore, RoundTripsAndNeverWritesTheSecretInTheClear) {
    TempFile f;
    const auto key = DeriveSwapStoreKey(MasterKey(0x11));
    EncryptedFileSwapStore(f.path, key).Save(AliceSession());
    const std::string disk = Slurp(f.path);
    EXPECT_EQ(disk.rfind("dinswap1e", 0), 0u);
    EXPECT_EQ(disk.find(detail::ToHex(std::vector<uint8_t>(kSecret.begin(), kSecret.end()))), std::string::npos);
    EXPECT_EQ(disk.find("dinswap1r"), std::string::npos) << "no plaintext record";
    EXPECT_EQ(disk.find("dinswap1o"), std::string::npos) << "no plaintext offer";
    const auto back = EncryptedFileSwapStore::Load(f.path, key);
    EXPECT_EQ(EncodeSession(back), EncodeSession(AliceSession()));
}

TEST(SwapEncryptedStore, WrongKeyOrTamperingIsRefused) {
    TempFile f;
    const auto key = DeriveSwapStoreKey(MasterKey(0x11));
    EncryptedFileSwapStore(f.path, key).Save(AliceSession());
    EXPECT_THROW(EncryptedFileSwapStore::Load(f.path, DeriveSwapStoreKey(MasterKey(0x12))), std::runtime_error);

    std::string disk = Slurp(f.path);
    disk[disk.size() / 2] = disk[disk.size() / 2] == 'a' ? 'b' : 'a';
    std::ofstream(f.path, std::ios::trunc) << disk;
    EXPECT_THROW(EncryptedFileSwapStore::Load(f.path, key), std::runtime_error);
}

TEST(SwapEncryptedStore, KeyDerivationMatchesHmacSha256) {
    // Independent vector: Python hmac.new(b"\\x11"*32, b"dinero/swap-store/v1", sha256).
    const auto k = DeriveSwapStoreKey(MasterKey(0x11));
    EXPECT_EQ(detail::ToHex(std::vector<uint8_t>(k.begin(), k.end())), "cfb39e8a9c0bdf709a844cc19b0d623b92b18185e6d83ee0e3d18b5d9f0389e7");
}

TEST(SwapEncryptedStore, FreshNonceEverySaveAndKeyIsNotTheMasterKey) {
    TempFile f;
    const auto master = MasterKey(0x11);
    const auto key = DeriveSwapStoreKey(master);
    EXPECT_NE(key, master);
    EXPECT_NE(DeriveSwapStoreKey(MasterKey(0x12)), key);
    EncryptedFileSwapStore store(f.path, key);
    store.Save(AliceSession());
    const std::string first = Slurp(f.path);
    store.Save(AliceSession());
    EXPECT_NE(Slurp(f.path), first) << "same session, different ciphertext";
}

TEST(SwapEncryptedStore, RepeatedSavesReplaceTheFileAndReopenToTheLastState) {
    // Every tick may save: the replace must work over an existing file on
    // every platform (Windows rename refuses that), and leave no temp file.
    TempFile f;
    const auto key = DeriveSwapStoreKey(MasterKey(0x21));
    EncryptedFileSwapStore store(f.path, key);
    auto s = AliceSession();
    for (uint32_t i = 1; i <= 5; ++i) {
        s.btc_scan_from_height = 100 + i;
        ASSERT_NO_THROW(store.Save(s)) << "save " << i;
        EXPECT_EQ(EncryptedFileSwapStore::Load(f.path, key).btc_scan_from_height, 100 + i) << "reopen after save " << i;
    }
    EXPECT_FALSE(std::filesystem::exists(f.path + ".tmp"));
    EXPECT_EQ(EncodeSession(EncryptedFileSwapStore::Load(f.path, key)), EncodeSession(s));

    const std::string plain = f.path + ".txt";
    for (const char* text : {"first\n", "second, longer\n", "3\n"}) {
        ASSERT_NO_THROW(detail::WriteFileAtomically(plain, text));
        EXPECT_EQ(Slurp(plain), text);
    }
    EXPECT_FALSE(std::filesystem::exists(plain + ".tmp"));
    std::remove(plain.c_str());
}


TEST(SwapEncryptedStore, FundingBindingIsEncryptedAndSurvivesReopen) {
    TempFile f;const auto key=DeriveSwapStoreKey(MasterKey(0x31));auto session=AliceSession();
    DinFundingBinding binding;binding.wallet_id.fill(0x42);binding.fee_rate_hint=7;binding.maximum_fee_una=12345;
    session.din_funding_binding=binding;
    EncryptedFileSwapStore(f.path,key).Save(session);const auto disk=Slurp(f.path);
    EXPECT_EQ(disk.find("din_funding_wallet_id="),std::string::npos);
    EXPECT_EQ(disk.find(detail::ToHex(std::vector<uint8_t>(binding.wallet_id.begin(),binding.wallet_id.end()))),std::string::npos);
    const auto loaded=EncryptedFileSwapStore::Load(f.path,key);
    ASSERT_TRUE(loaded.din_funding_binding);EXPECT_EQ(*loaded.din_funding_binding,binding);
    EXPECT_EQ(OfferId(loaded.record.offer),OfferId(session.record.offer));
    EXPECT_EQ(EncodeSession(loaded),EncodeSession(session));
    EXPECT_THROW(EncryptedFileSwapStore::Load(f.path,DeriveSwapStoreKey(MasterKey(0x32))),std::runtime_error);
}
TEST(SwapEncryptedStore, FundingBindingLegacyAbsenceAndMalformedFields) {
    const auto legacy=EncodeSession(AliceSession());EXPECT_FALSE(DecodeSession(legacy).din_funding_binding);
    const std::string wallet="din_funding_wallet_id="+std::string(64,'4')+"\n";
    const std::string rate="din_funding_fee_rate_hint=7\n",ceiling="din_funding_maximum_fee_una=12345\n";
    for(const auto& partial:{wallet,rate,ceiling,wallet+rate,wallet+ceiling,rate+ceiling})
        EXPECT_THROW(DecodeSession(legacy+partial),std::invalid_argument);
    for(const auto& bad:{std::string(),std::string(64,'0'),std::string(63,'4'),std::string(64,'G')})
        EXPECT_THROW(DecodeSession(legacy+"din_funding_wallet_id="+bad+"\n"+rate+ceiling),std::invalid_argument);
    for(const char* bad:{"0","-1","+7","07","1.0","18446744073709551616","26542800000000001","7x"}) {
        EXPECT_THROW(DecodeSession(legacy+wallet+"din_funding_fee_rate_hint="+bad+"\n"+ceiling),std::invalid_argument);
        EXPECT_THROW(DecodeSession(legacy+wallet+rate+"din_funding_maximum_fee_una="+bad+"\n"),std::invalid_argument);
    }
    EXPECT_THROW(DecodeSession(legacy+wallet+rate+ceiling+wallet),std::invalid_argument);
    const auto valid=DecodeSession(legacy+wallet+rate+ceiling);ASSERT_TRUE(valid.din_funding_binding);
    EXPECT_EQ(valid.din_funding_binding->fee_rate_hint,7u);EXPECT_EQ(valid.din_funding_binding->maximum_fee_una,12345u);
}
TEST(SwapEncryptedStore, InvalidFundingBindingCannotReplaceEstablishedSession) {
    TempFile f;const auto key=DeriveSwapStoreKey(MasterKey(0x41));auto session=AliceSession();
    DinFundingBinding binding;binding.wallet_id.fill(0x43);binding.fee_rate_hint=2;binding.maximum_fee_una=5000;
    session.din_funding_binding=binding;EncryptedFileSwapStore store(f.path,key);store.Save(session);
    const auto original=Slurp(f.path);
    for(int mode=0;mode<4;++mode) {
        auto invalid=session;
        if(mode==0)invalid.din_funding_binding->wallet_id={};
        if(mode==1)invalid.din_funding_binding->fee_rate_hint=0;
        if(mode==2)invalid.din_funding_binding->maximum_fee_una=0;
        if(mode==3)invalid.record.role=Role::BtcSeller;
        EXPECT_THROW(store.Save(invalid),std::invalid_argument);EXPECT_EQ(Slurp(f.path),original);
        EXPECT_EQ(EncryptedFileSwapStore::Load(f.path,key).din_funding_binding,session.din_funding_binding);
    }
}

}  // namespace
