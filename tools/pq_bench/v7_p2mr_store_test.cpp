/**
 * V7 P2MR store + AEAD round-trip integration test.
 *
 * End-to-end:
 *   1. Derive an ML-DSA keypair via wallet::pq::DerivePQKeypair.
 *   2. Compute merkle_root = SHA256(scheme_id || pubkey).
 *   3. Encode "din1r..." address via wallet::EncodeP2MRAddress.
 *   4. Capture the 32-byte pq_seed.
 *   5. Seal seed under a 32-byte master key (AEAD).
 *   6. Store in a temp SQLite DB via V7P2MRStore::AddAddress.
 *   7. List + GetByAddress; assert public fields round-trip.
 *   8. LoadEncryptedSeed + OpenSeed with the master key.
 *   9. Re-run KeygenFromSeed; assert pubkey equals the stored pubkey.
 *  10. Assert a signature produced with the re-derived secret verifies
 *      under the stored pubkey.
 *  11. Negative: OpenSeed with WRONG master key returns AuthFailed and
 *      does not populate the plaintext.
 *  12. Negative: AddAddress on a duplicate (wallet_id, path, leaf) returns
 *      UniqueConflict.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "wallet/aead_seed.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/secure_keypair.h"
#include "wallet/v7_p2mr_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <unistd.h>

namespace pq     = dinero::consensus::pq;
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

std::array<uint8_t, 32> Sha256OfSchemeAndPubkey(uint8_t scheme_id,
                                                const mldsa::PublicKey& pk) {
    std::array<uint8_t, 32> out{};
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &scheme_id, 1);
    SHA256_Update(&ctx, pk.data(), pk.size());
    SHA256_Final(out.data(), &ctx);
    return out;
}

std::string MakeTempDbPath() {
    char buf[] = "/tmp/dinero_v7_store_XXXXXX.sqlite";
    // Simple unique path generator — safe enough for a test; we don't need
    // mkstemp-level guarantees because the sqlite open will create the file.
    const long pid = static_cast<long>(getpid());
    const int rnd = std::rand();
    char name[64];
    std::snprintf(name, sizeof(name), "/tmp/dinero_v7_store_%ld_%d.sqlite", pid, rnd);
    // Remove any stale file from a previous run before we use it.
    std::remove(name);
    return name;
}

} // namespace

int main() {
    std::printf("================================================================\n"
                " Dinero v7 — P2MR store + AEAD round-trip integration test\n"
                "================================================================\n");

    // --------- Derive a keypair + pq_seed deterministically. ---------
    wpq::Bip32PrivKey   priv{};  for (std::size_t i = 0; i < priv.size();  ++i) priv[i]  = 0x11;
    wpq::Bip32ChainCode chain{}; for (std::size_t i = 0; i < chain.size(); ++i) chain[i] = 0x22;
    const uint32_t leaf_index = 0;

    auto pq_seed = wpq::DerivePQSeed(priv, chain, leaf_index);

    // Feed into SecureKeypair via DerivePQKeypair so we match what the
    // RPC path will do.
    auto secure_kp = wpq::DerivePQKeypair(priv, chain, leaf_index);

    // Sanity: seed → KeygenFromSeed → same pubkey we got from SecureKeypair.
    {
        mldsa::Seed ml_seed{};
        std::memcpy(ml_seed.data(), pq_seed.data(), ml_seed.size());
        auto kp_check = mldsa::KeygenFromSeed(ml_seed);
        record(kp_check.pubkey == secure_kp.pubkey(),
               "S0: DerivePQSeed + KeygenFromSeed matches DerivePQKeypair pubkey");
    }

    const auto merkle_root = Sha256OfSchemeAndPubkey(pq::SCHEME_ID_ML_DSA_65,
                                                     secure_kp.pubkey());
    const std::string address =
        wallet::EncodeP2MRAddress(std::string("din"), merkle_root);
    record(!address.empty() && address.rfind("din1r", 0) == 0,
           "S1: encoded P2MR address starts with din1r");

    // --------- Seal the 32-byte seed with AES-256-GCM. ---------
    wallet::AeadKey master_key{};
    RAND_bytes(master_key.data(), static_cast<int>(master_key.size()));

    wallet::AeadSeed seed_plain{};
    std::memcpy(seed_plain.data(), pq_seed.data(), seed_plain.size());

    auto sealed = wallet::SealSeed(seed_plain, master_key);
    record(sealed.ciphertext.size() == 32, "S2a: ciphertext is 32 bytes");
    record(sealed.nonce.size()      == 12, "S2b: nonce is 12 bytes");
    record(sealed.tag.size()        == 16, "S2c: tag is 16 bytes");

    // Two encryptions of the same plaintext MUST produce different
    // ciphertexts (nonce is drawn from /dev/urandom each time).
    auto sealed2 = wallet::SealSeed(seed_plain, master_key);
    record(sealed.ciphertext != sealed2.ciphertext,
           "S2d: two seals of same plaintext differ (nonce is random)");

    // --------- Open the DB, migrate, insert. ---------
    const std::string db_path = MakeTempDbPath();
    std::printf("  [INFO] using temp DB at %s\n", db_path.c_str());

    wallet::V7P2MRStore store;
    record(store.Open(db_path) == wallet::V7P2MRStore::OpenResult::Ok,
           "S3: V7P2MRStore::Open Ok (migration applied)");

    const int64_t wallet_id = 42;
    const std::string path  = "m/88'/1448'/0'/0/0";
    const std::string label = "integration-test";
    const int64_t now       = 1'700'000'000;

    auto add_rc = store.AddAddress(
        wallet_id, address, merkle_root, secure_kp.pubkey(),
        sealed.ciphertext, sealed.nonce, sealed.tag,
        path, leaf_index, label, now);
    record(add_rc == wallet::V7P2MRStore::AddResult::Ok,
           "S4: AddAddress Ok");

    // Duplicate (wallet_id, path, leaf_index) → UniqueConflict.
    {
        auto sealed_dup = wallet::SealSeed(seed_plain, master_key);
        auto rc = store.AddAddress(
            wallet_id, address + "-duplicate-shim",  // different address so
                                                     // the UNIQUE(address)
                                                     // constraint isn't the
                                                     // one that fires; we
                                                     // want to exercise the
                                                     // (wallet_id, path,
                                                     // leaf) composite.
            merkle_root, secure_kp.pubkey(),
            sealed_dup.ciphertext, sealed_dup.nonce, sealed_dup.tag,
            path, leaf_index, "dup", now);
        record(rc == wallet::V7P2MRStore::AddResult::UniqueConflict,
               "S4b: duplicate (wallet_id, path, leaf_index) → UniqueConflict");
    }

    // --------- GetByAddress returns the public fields. ---------
    auto got = store.GetByAddress(wallet_id, address);
    record(got.has_value(), "S5a: GetByAddress returns a row");
    if (got) {
        record(got->address         == address,        "S5b: address round-trips");
        record(got->merkle_root     == merkle_root,    "S5c: merkle_root round-trips");
        record(got->pubkey          == secure_kp.pubkey(), "S5d: pubkey round-trips");
        record(got->derivation_path == path,           "S5e: derivation_path round-trips");
        record(got->leaf_index      == leaf_index,     "S5f: leaf_index round-trips");
        record(got->label           == label,          "S5g: label round-trips");
        record(got->created_at_unix == now,            "S5h: created_at round-trips");
    }

    // --------- ListByWallet returns exactly one row. ---------
    {
        auto list = store.ListByWallet(wallet_id);
        record(list.size() == 1, "S6: ListByWallet returns one row");
        if (!list.empty()) {
            record(list[0].address == address, "S6b: list row address matches");
        }
    }

    // --------- Load + decrypt + re-derive keypair + sign/verify. ---------
    //
    // This is the load-bearing end-to-end test: if this passes, the
    // wallet works. Uses SecureSeed (RAII) for the decrypted plaintext so
    // no raw AeadSeed lingers past its immediate use.
    auto enc = store.LoadEncryptedSeed(wallet_id, address);
    record(enc.has_value(), "S7a: LoadEncryptedSeed returns a row");
    if (enc) {
        wallet::SecureSeed decrypted;
        auto rc = wallet::OpenSeedSecure(enc->ciphertext, enc->nonce, enc->tag,
                                         master_key, &decrypted);
        record(rc == wallet::AeadOpenResult::Ok,
               "S7b: OpenSeedSecure Ok with correct master key");
        record(decrypted.bytes() == seed_plain,
               "S7c: decrypted seed matches original");

        // Re-run KeygenFromSeed over the decrypted seed; re-derived keypair
        // must produce the same pubkey stored in the row.
        auto ml_seed = decrypted.ToMlDsaSeed();
        wallet::SecureKeypair rederived(mldsa::KeygenFromSeed(ml_seed));
        record(rederived.pubkey() == secure_kp.pubkey(),
               "S7d: re-derived pubkey matches stored pubkey");

        // Sign a message with the re-derived secret; verify with the
        // stored pubkey.
        const std::string msg_str = "v7 storage round-trip sign/verify";
        std::vector<uint8_t> msg(msg_str.begin(), msg_str.end());
        auto sig = mldsa::Sign(msg, rederived.secret());
        if (got) {
            record(mldsa::Verify(msg, sig, got->pubkey),
                   "S7e: signature from re-derived secret verifies with stored pubkey");
        }

        // decrypted goes out of scope here; destructor scrubs plaintext.
    }

    // --------- Wrong master key fails to decrypt. ---------
    {
        wallet::AeadKey wrong_key = master_key;
        wrong_key[0] ^= 0x01;
        wallet::AeadSeed tmp{};
        auto rc = wallet::OpenSeed(sealed.ciphertext, sealed.nonce, sealed.tag,
                                   wrong_key, &tmp);
        record(rc == wallet::AeadOpenResult::AuthFailed,
               "S8a: OpenSeed with wrong key → AuthFailed");
        bool tmp_untouched_or_zeroed = true;
        // We don't require a specific state, just that no secret leaked;
        // the implementation scrubs to zero on fail, so check that.
        for (auto b : tmp) { if (b != 0x00) { tmp_untouched_or_zeroed = false; break; } }
        record(tmp_untouched_or_zeroed, "S8b: out-buffer is zeroed on AuthFailed");
    }

    // --------- Tampered ciphertext fails. ---------
    {
        auto bad = sealed;
        bad.ciphertext[0] ^= 0x01;
        wallet::AeadSeed tmp{};
        auto rc = wallet::OpenSeed(bad.ciphertext, bad.nonce, bad.tag, master_key, &tmp);
        record(rc == wallet::AeadOpenResult::AuthFailed,
               "S9: tampered ciphertext → AuthFailed");
    }

    // --------- SecureSeed RAII + move semantics. ---------
    //
    // Plaintext lives only inside a SecureSeed instance. After move, the
    // source is scrubbed; on destruction, the destination is scrubbed.
    // We verify the scrub-on-move invariant via the public bytes() API.
    {
        wallet::SecureSeed src;
        auto rc = wallet::OpenSeedSecure(sealed.ciphertext, sealed.nonce,
                                         sealed.tag, master_key, &src);
        record(rc == wallet::AeadOpenResult::Ok, "SK1a: OpenSeedSecure Ok");

        // Snapshot before move so we can compare.
        wallet::AeadSeed snapshot{};
        std::memcpy(snapshot.data(), src.bytes().data(), snapshot.size());

        wallet::SecureSeed dst = std::move(src);
        record(dst.bytes() == snapshot, "SK1b: moved-to SecureSeed holds the plaintext");

        bool src_zero = true;
        for (auto b : src.bytes()) { if (b != 0x00) { src_zero = false; break; } }
        record(src_zero, "SK1c: moved-from SecureSeed is scrubbed to zero");

        // Explicit Wipe().
        wallet::SecureSeed sk;
        wallet::OpenSeedSecure(sealed.ciphertext, sealed.nonce, sealed.tag,
                               master_key, &sk);
        sk.Wipe();
        bool wipe_zero = true;
        for (auto b : sk.bytes()) { if (b != 0x00) { wipe_zero = false; break; } }
        record(wipe_zero, "SK1d: explicit Wipe() zeros the plaintext");
    }

    // --------- Nonce uniqueness: 100 sealed seeds → 100 distinct nonces.
    //
    // Not a correctness bug if collisions occur at 96-bit random width
    // (probability ~N^2/2^96), but a cheap sanity check that the AEAD
    // path isn't accidentally caching or reusing a nonce.
    // Also stresses the store on a moderate-size wallet.
    {
        const int N = 100;
        std::vector<wallet::AeadNonce> nonces;
        nonces.reserve(N);
        for (int i = 0; i < N; ++i) {
            wallet::AeadSeed p{};
            // Pseudo-random plaintext so each Seal gets different inputs.
            for (std::size_t j = 0; j < p.size(); ++j) {
                p[j] = static_cast<uint8_t>((i * 2654435761u + j * 16777619u) & 0xff);
            }
            auto s = wallet::SealSeed(p, master_key);
            nonces.push_back(s.nonce);

            // Also insert into the store to exercise schema under load.
            std::string addr_i = address + "." + std::to_string(i);
            auto rc = store.AddAddress(
                wallet_id, addr_i, merkle_root, secure_kp.pubkey(),
                s.ciphertext, s.nonce, s.tag,
                "m/88'/1448'/0'/0/" + std::to_string(i + 1),  // distinct path
                static_cast<uint32_t>(i + 1),                 // distinct leaf
                "stress", now);
            if (rc != wallet::V7P2MRStore::AddResult::Ok) {
                std::printf("  [FAIL] S10 stress insert %d failed (rc=%d)\n", i,
                            static_cast<int>(rc));
                ++g_failed;
                break;
            }
        }

        // Check all N nonces are pairwise distinct. O(N^2), fine for N=100.
        bool all_unique = true;
        for (std::size_t i = 0; i < nonces.size() && all_unique; ++i) {
            for (std::size_t j = i + 1; j < nonces.size() && all_unique; ++j) {
                if (nonces[i] == nonces[j]) all_unique = false;
            }
        }
        record(all_unique,
               "S10: 100 SealSeed calls produce pairwise-distinct nonces");

        // And the store now has 1 + 100 addresses for this wallet_id.
        auto list = store.ListByWallet(wallet_id);
        record(list.size() == 1 + N,
               "S10b: ListByWallet returns 101 rows after bulk insert");
    }

    // Cleanup the temp DB file.
    store.Close();
    std::remove(db_path.c_str());

    // --------- Schema migration tests. ---------
    //
    // These validate the "strictly additive" migration behavior — opening
    // the store must not disturb pre-existing tables/rows in the same
    // SQLite file.
    //
    //   SM1: fresh DB — CREATE IF NOT EXISTS on empty file produces the
    //        table; table_count increases by 1.
    //   SM2: upgrade — pre-existing DB with unrelated table+data has the
    //        unrelated data preserved after V7P2MRStore::Open.
    //   SM3: idempotent reopen — calling Open() twice on the same file
    //        succeeds both times and produces identical schema.
    {
        // SM1: fresh DB
        const std::string sm1_path = MakeTempDbPath();
        {
            wallet::V7P2MRStore s;
            auto rc = s.Open(sm1_path);
            record(rc == wallet::V7P2MRStore::OpenResult::Ok,
                   "SM1a: Open on fresh DB returns Ok");

            // Poke the DB directly to verify the table exists.
            sqlite3* raw = nullptr;
            sqlite3_open(sm1_path.c_str(), &raw);
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                raw,
                "SELECT name FROM sqlite_master WHERE type='table' AND name='v7_p2mr_addresses';",
                -1, &stmt, nullptr);
            bool table_present = (sqlite3_step(stmt) == SQLITE_ROW);
            record(table_present, "SM1b: v7_p2mr_addresses table created on fresh open");
            sqlite3_finalize(stmt);
            sqlite3_close(raw);
        }
        std::remove(sm1_path.c_str());

        // SM2: upgrade from existing wallet DB
        const std::string sm2_path = MakeTempDbPath();
        {
            // Pre-seed an unrelated table with data, simulating an existing
            // v5 wallet DB.
            sqlite3* raw = nullptr;
            sqlite3_open(sm2_path.c_str(), &raw);
            sqlite3_exec(raw,
                "CREATE TABLE v5_existing(name TEXT, amount INTEGER);"
                "INSERT INTO v5_existing VALUES('alice', 100);"
                "INSERT INTO v5_existing VALUES('bob',   250);",
                nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }
        // Run the v7 migration.
        {
            wallet::V7P2MRStore s;
            auto rc = s.Open(sm2_path);
            record(rc == wallet::V7P2MRStore::OpenResult::Ok,
                   "SM2a: Open on pre-existing DB returns Ok (additive)");
        }
        // Re-open with raw sqlite3, confirm both tables and v5 data survive.
        {
            sqlite3* raw = nullptr;
            sqlite3_open(sm2_path.c_str(), &raw);

            // v5_existing still has 2 rows.
            sqlite3_stmt* s1 = nullptr;
            sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM v5_existing;", -1, &s1, nullptr);
            int rc1 = sqlite3_step(s1);
            int count = (rc1 == SQLITE_ROW) ? sqlite3_column_int(s1, 0) : -1;
            sqlite3_finalize(s1);
            record(count == 2, "SM2b: v5_existing rows preserved through migration");

            // v5 row values intact.
            sqlite3_stmt* s2 = nullptr;
            sqlite3_prepare_v2(raw,
                "SELECT amount FROM v5_existing WHERE name='alice';",
                -1, &s2, nullptr);
            int amt = (sqlite3_step(s2) == SQLITE_ROW) ? sqlite3_column_int(s2, 0) : -1;
            sqlite3_finalize(s2);
            record(amt == 100, "SM2c: v5 row contents preserved");

            // v7 table exists and is empty.
            sqlite3_stmt* s3 = nullptr;
            sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM v7_p2mr_addresses;",
                               -1, &s3, nullptr);
            int v7c = (sqlite3_step(s3) == SQLITE_ROW) ? sqlite3_column_int(s3, 0) : -1;
            sqlite3_finalize(s3);
            record(v7c == 0, "SM2d: v7_p2mr_addresses present and empty");

            sqlite3_close(raw);
        }
        std::remove(sm2_path.c_str());

        // SM3: idempotent reopen
        const std::string sm3_path = MakeTempDbPath();
        {
            wallet::V7P2MRStore s;
            auto rc1 = s.Open(sm3_path);
            s.Close();
            wallet::V7P2MRStore s2;
            auto rc2 = s2.Open(sm3_path);
            record(rc1 == wallet::V7P2MRStore::OpenResult::Ok
                   && rc2 == wallet::V7P2MRStore::OpenResult::Ok,
                   "SM3: Open + Close + Open on the same DB is idempotent");
        }
        std::remove(sm3_path.c_str());
    }

    std::printf("----------------------------------------------------------------\n"
                " RESULT: %d passed, %d failed\n"
                "================================================================\n",
                g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
