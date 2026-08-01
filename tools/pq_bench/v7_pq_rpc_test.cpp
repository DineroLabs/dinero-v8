/**
 * V7 PQ wallet RPC handler integration test.
 *
 * Exercises every handler end-to-end plus each documented error branch.
 * Layout follows the user's acceptance criteria for 4c.3:
 *
 *   Happy path: the five RPCs compose into a full wallet session.
 *   Negative: bad input / duplicate / unknown address / decrypt failed.
 *   Discipline: no seed material on non-export paths; zeroize-on-fail.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "rpc/v7_pq_handlers.h"
#include "wallet/aead_seed.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/v7_p2mr_store.h"
#include "crypto/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <unistd.h>

namespace rpc    = dinero::rpc::v7;
namespace mldsa  = dinero::consensus::pq::ml_dsa_65;
namespace wallet = dinero::wallet;
namespace wpq    = dinero::wallet::pq;

namespace {

int g_failed = 0;
int g_passed = 0;

void record(bool cond, const char* tag) {
    if (cond) { ++g_passed; std::printf("  [PASS] %s\n", tag); }
    else      { ++g_failed; std::printf("  [FAIL] %s\n", tag); }
}

std::string MakeTempDbPath() {
    char name[96];
    std::snprintf(name, sizeof(name), "/tmp/dinero_v7_rpc_%ld_%d.sqlite",
                  (long)getpid(), std::rand());
    std::remove(name);
    return name;
}

// Deterministic BIP-32 material + master key for reproducibility.
wpq::Bip32PrivKey   MakePriv()       { wpq::Bip32PrivKey   p{}; for (auto& b : p) b = 0x11; return p; }
wpq::Bip32ChainCode MakeChain()      { wpq::Bip32ChainCode c{}; for (auto& b : c) b = 0x22; return c; }
wallet::AeadKey     MakeMasterKey()  { wallet::AeadKey k{};   for (auto& b : k) b = 0xAB; return k; }

} // namespace

int main() {
    std::printf("================================================================\n"
                " Dinero v7 — PQ wallet RPC handler integration test\n"
                "================================================================\n");

    const std::string db_path = MakeTempDbPath();
    wallet::V7P2MRStore store;
    record(store.Open(db_path) == wallet::V7P2MRStore::OpenResult::Ok,
           "R0: store.Open Ok");

    const int64_t wallet_id = 7;
    const int64_t now       = 1'700'000'000;
    const wallet::AeadKey master_key = MakeMasterKey();

    // -----------------------------------------------------------------------
    // R1: GetNewP2MRAddress — happy path
    // -----------------------------------------------------------------------
    rpc::GetNewP2MRAddressResult r1;
    std::string r1_address;
    std::array<uint8_t, mldsa::PUBKEY_BYTES> r1_pubkey{};
    {
        rpc::GetNewP2MRAddressParams p{};
        p.wallet_id     = wallet_id;
        p.hrp           = "din";
        p.account       = 0;
        p.change        = 0;
        p.address_index = 0;
        p.leaf_index    = 0;
        p.label         = "primary";
        p.now_unix      = now;
        p.bip32_priv    = MakePriv();
        p.bip32_chain   = MakeChain();
        p.master_key    = master_key;

        r1 = rpc::GetNewP2MRAddress(store, p);
        record(r1.status == rpc::HandlerStatus::Ok, "R1a: GetNewP2MRAddress Ok");
        record(!r1.address.empty() && r1.address.rfind("din1r", 0) == 0,
               "R1b: address starts with 'din1r'");
        record(r1.derivation_path == "m/88'/1448'/0'/0/0",
               "R1c: derivation_path matches v7 convention");
        r1_address = r1.address;
        r1_pubkey  = r1.pubkey;
    }

    // -----------------------------------------------------------------------
    // R2: ListP2MRAddresses — one entry; no secret material exposed
    //     (struct has no secret fields by design; we just verify shape)
    // -----------------------------------------------------------------------
    {
        rpc::ListP2MRAddressesParams p{};
        p.wallet_id = wallet_id;
        auto r2 = rpc::ListP2MRAddresses(store, p);
        record(r2.status == rpc::HandlerStatus::Ok,  "R2a: List Ok");
        record(r2.entries.size() == 1,               "R2b: one entry after R1");
        if (!r2.entries.empty()) {
            record(r2.entries[0].address == r1_address,  "R2c: listed address matches");
            record(r2.entries[0].pubkey  == r1_pubkey,   "R2d: listed pubkey matches");
            record(r2.entries[0].label   == "primary",   "R2e: listed label matches");
        }
    }

    // -----------------------------------------------------------------------
    // R3: SignP2MR — happy path + verifier accepts
    // -----------------------------------------------------------------------
    {
        rpc::SignP2MRParams p{};
        p.wallet_id  = wallet_id;
        p.address    = r1_address;
        std::string tag = "v7 sighash canary";
        // Trivial test sighash = SHA256(tag).
        dinero::crypto::CSHA256()
            .Write(tag)
            .Finalize(p.sighash.data());
        p.master_key = master_key;

        auto r3 = rpc::SignP2MR(store, p);
        record(r3.status == rpc::HandlerStatus::Ok,  "R3a: SignP2MR Ok");
        record(r3.scheme_id == dinero::consensus::pq::SCHEME_ID_ML_DSA_65,
               "R3b: scheme_id == 0x01 (ML-DSA-65)");
        // Verify with returned pubkey.
        record(r3.pubkey == r1_pubkey, "R3c: signing pubkey matches stored pubkey");
        record(mldsa::Verify(p.sighash.data(),  p.sighash.size(),
                             r3.signature.data(), r3.signature.size(),
                             r3.pubkey.data(),    r3.pubkey.size()),
               "R3d: signature verifies against returned pubkey");
    }

    // -----------------------------------------------------------------------
    // R4: SignP2MR — wrong master key → DecryptFailed; no pubkey/sig leak
    // -----------------------------------------------------------------------
    {
        rpc::SignP2MRParams p{};
        p.wallet_id  = wallet_id;
        p.address    = r1_address;
        p.master_key = master_key;
        p.master_key[0] ^= 0x01;   // flip a bit → wrong key
        // sighash doesn't matter here

        auto r4 = rpc::SignP2MR(store, p);
        record(r4.status == rpc::HandlerStatus::DecryptFailed,
               "R4a: wrong master key → DecryptFailed");
        bool sig_all_zero = true;
        for (auto b : r4.signature) { if (b != 0) { sig_all_zero = false; break; } }
        record(sig_all_zero, "R4b: signature is zero on failure path");
    }

    // -----------------------------------------------------------------------
    // R5: SignP2MR — unknown address → AddressNotFound
    // -----------------------------------------------------------------------
    {
        rpc::SignP2MRParams p{};
        p.wallet_id  = wallet_id;
        // Unrelated valid bech32m P2MR address (random root bytes).
        std::array<uint8_t, 32> other_root{};
        for (auto& b : other_root) b = 0xA7;
        p.address    = wallet::EncodeP2MRAddress(std::string("din"), other_root);
        p.master_key = master_key;

        auto r5 = rpc::SignP2MR(store, p);
        record(r5.status == rpc::HandlerStatus::AddressNotFound,
               "R5: unknown address → AddressNotFound");
    }

    // -----------------------------------------------------------------------
    // R6: SignP2MR — malformed address → InvalidParams
    // -----------------------------------------------------------------------
    {
        rpc::SignP2MRParams p{};
        p.wallet_id  = wallet_id;
        p.address    = "not-a-valid-bech32m-address";
        p.master_key = master_key;

        auto r6 = rpc::SignP2MR(store, p);
        record(r6.status == rpc::HandlerStatus::InvalidParams,
               "R6: malformed address → InvalidParams");
    }

    // -----------------------------------------------------------------------
    // R7: GetNewP2MRAddress — duplicate (path, leaf) → UniqueConflict
    // -----------------------------------------------------------------------
    {
        rpc::GetNewP2MRAddressParams p{};
        p.wallet_id     = wallet_id;
        p.hrp           = "din";
        p.account       = 0;
        p.change        = 0;
        p.address_index = 0;        // same as R1
        p.leaf_index    = 0;
        p.label         = "dup";
        p.now_unix      = now + 1;
        p.bip32_priv    = MakePriv();
        p.bip32_chain   = MakeChain();
        p.master_key    = master_key;

        auto r7 = rpc::GetNewP2MRAddress(store, p);
        record(r7.status == rpc::HandlerStatus::UniqueConflict,
               "R7: duplicate (wallet_id, path, leaf_index) → UniqueConflict");
    }

    // -----------------------------------------------------------------------
    // R8: ImportP2MRSeed — import a fresh seed; new address appears in list
    // -----------------------------------------------------------------------
    std::string r8_address;
    mldsa::Seed imported_seed{};
    for (std::size_t i = 0; i < imported_seed.size(); ++i) imported_seed[i] = static_cast<uint8_t>(0x55 + i);
    {
        rpc::ImportP2MRSeedParams p{};
        p.wallet_id       = wallet_id;
        p.hrp             = "din";
        p.pq_seed         = imported_seed;
        p.derivation_path = "external-import/0";
        p.leaf_index      = 0;
        p.label           = "imported";
        p.now_unix        = now + 10;
        p.master_key      = master_key;

        auto r8 = rpc::ImportP2MRSeed(store, p);
        record(r8.status == rpc::HandlerStatus::Ok, "R8a: ImportP2MRSeed Ok");
        record(!r8.address.empty() && r8.address.rfind("din1r", 0) == 0,
               "R8b: imported address starts with din1r");
        r8_address = r8.address;
    }
    {
        // List should now have 2 addresses.
        rpc::ListP2MRAddressesParams p{}; p.wallet_id = wallet_id;
        auto r = rpc::ListP2MRAddresses(store, p);
        record(r.entries.size() == 2, "R8c: list has two entries after import");
    }

    // -----------------------------------------------------------------------
    // R9: ExportP2MRSeed — recovers the exact seed we imported
    // -----------------------------------------------------------------------
    {
        rpc::ExportP2MRSeedParams p{};
        p.wallet_id  = wallet_id;
        p.address    = r8_address;
        p.master_key = master_key;

        auto r9 = rpc::ExportP2MRSeed(store, p);
        record(r9.status == rpc::HandlerStatus::Ok,
               "R9a: ExportP2MRSeed Ok");
        record(r9.pq_seed == imported_seed,
               "R9b: exported seed matches imported seed byte-for-byte");

        // Scrub the seed the test is holding — best-effort hygiene
        // analog to what Qt/RPC dispatcher will do after display.
        std::memset(r9.pq_seed.data(), 0, r9.pq_seed.size());
    }

    // -----------------------------------------------------------------------
    // R10: ExportP2MRSeed — wrong master key → DecryptFailed; no seed leak
    // -----------------------------------------------------------------------
    {
        rpc::ExportP2MRSeedParams p{};
        p.wallet_id  = wallet_id;
        p.address    = r8_address;
        p.master_key = master_key;
        p.master_key[5] ^= 0x01;

        auto r10 = rpc::ExportP2MRSeed(store, p);
        record(r10.status == rpc::HandlerStatus::DecryptFailed,
               "R10a: wrong master key → DecryptFailed");
        bool seed_all_zero = true;
        for (auto b : r10.pq_seed) { if (b != 0) { seed_all_zero = false; break; } }
        record(seed_all_zero, "R10b: pq_seed is zero on decrypt failure");
    }

    // -----------------------------------------------------------------------
    // R11: Sign on the imported address — derivation consistency.
    //      Proves ImportP2MRSeed stores enough state that SignP2MR can
    //      re-derive and produce a valid signature.
    // -----------------------------------------------------------------------
    {
        rpc::SignP2MRParams p{};
        p.wallet_id  = wallet_id;
        p.address    = r8_address;
        std::string tag = "imported address sign";
        dinero::crypto::CSHA256()
            .Write(tag)
            .Finalize(p.sighash.data());
        p.master_key = master_key;

        auto r11 = rpc::SignP2MR(store, p);
        record(r11.status == rpc::HandlerStatus::Ok,
               "R11a: Sign on imported address Ok");
        record(mldsa::Verify(p.sighash.data(),  p.sighash.size(),
                             r11.signature.data(), r11.signature.size(),
                             r11.pubkey.data(),    r11.pubkey.size()),
               "R11b: signature verifies for imported address");
    }

    // -----------------------------------------------------------------------
    // R12: ExportP2MRSeed — unknown address → AddressNotFound
    // -----------------------------------------------------------------------
    {
        rpc::ExportP2MRSeedParams p{};
        p.wallet_id  = wallet_id;
        std::array<uint8_t, 32> stranger_root{};
        for (auto& b : stranger_root) b = 0xCD;
        p.address    = wallet::EncodeP2MRAddress(std::string("din"), stranger_root);
        p.master_key = master_key;

        auto r12 = rpc::ExportP2MRSeed(store, p);
        record(r12.status == rpc::HandlerStatus::AddressNotFound,
               "R12: unknown address → AddressNotFound");
    }

    store.Close();
    std::remove(db_path.c_str());

    std::printf("----------------------------------------------------------------\n"
                " RESULT: %d passed, %d failed\n"
                "================================================================\n",
                g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
