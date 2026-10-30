// W4 wallet-file compatibility check (separation gate; spec section 8).
//
//   sep_wallet_fixture_compat <fixture-dir>
//
// The fixture dir (tests/separation/fixtures/off or /on, chosen by the
// build's Orchard configuration) was written by the baseline build's
// sep_make_wallet_fixture. This copies it to a temp dir, then:
//   1. dumps every DB's sqlite_master (raw, immutable read) and compares it
//      with schema.txt                                   (before the open);
//   2. opens the copy read-write with the current build's public APIs and
//      re-derives expected.json (addresses, watch script, descriptor,
//      balance, UTXOs, user_version and open_sqlite PRAGMAs per DB) and
//      compares it byte-for-byte;
//   3. dumps sqlite_master again and compares it with schema.txt (after);
//   4. compares, before vs after the open: the data-directory file set, every
//      persisted row (minus ExcludedColumns()) and the header fields; and
//      checks the registry's wallet path is exactly <datadir>/wallets/....
// Exit 1 on any difference or error (no assert: gates under NDEBUG too).

#include "wallet_fixture_common.h"

#include <cstdio>
#include <exception>
#include <iostream>
#include <random>

using namespace sepfx;

namespace {

fs::path MakeTempDir() {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        fs::path p = fs::temp_directory_path() / ("dinero-sep-w4-" + std::to_string(rd()) + std::to_string(rd()));
        std::error_code ec;
        if (fs::create_directory(p, ec)) return p;
    }
    Fail("cannot create temp dir");
}

struct TempDir {
    fs::path path = MakeTempDir();
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Print up to 10 differing lines (positional) of two texts.
bool Same(const std::string& what, const std::string& want, const std::string& got) {
    if (want == got) return true;
    std::istringstream a(want), b(got);
    std::string la, lb;
    size_t line = 0, shown = 0;
    while (shown < 10) {
        ++line;
        const bool ha = bool(std::getline(a, la)), hb = bool(std::getline(b, lb));
        if (!ha && !hb) break;
        if (!ha || !hb || la != lb) {
            std::fprintf(stderr, "W4 DIFF %s line %zu\n  fixture: %s\n  current: %s\n", what.c_str(), line,
                         ha ? la.c_str() : "<eof>", hb ? lb.c_str() : "<eof>");
            ++shown;
        }
    }
    return false;
}

size_t Lines(const std::string& s) { return size_t(std::count(s.begin(), s.end(), '\n')); }

// Runs on every platform before the real check: a registry row whose path has
// Windows backslashes (literal characters on POSIX) must read back equal to
// ExpectedRegistryWalletPath() for the same root, and a different path must
// not. Guards the exact-path comparison against encoding mismatches (PR #860
// run 1: the read side was Escape()d, doubling every backslash).
void SelfTestWalletPathEncoding(const fs::path& dir) {
    const fs::path fake_root = fs::path("C:\\Users\\sep x\\datadir");
    const fs::path reg = dir / "selftest_registry.db";
    const std::string want = ExpectedRegistryWalletPath(fake_root);
    Check(want.find('\\') != std::string::npos, "self-test path must contain a backslash");
    sqlite3* db = nullptr;
    Check(sqlite3_open(reg.string().c_str(), &db) == SQLITE_OK, "self-test: cannot create " + reg.string());
    sqlite3_stmt* q = nullptr;
    const bool ok = sqlite3_exec(db, "CREATE TABLE wallets(name TEXT, path TEXT)", nullptr, nullptr, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "INSERT INTO wallets VALUES('sepfixture', ?1)", -1, &q, nullptr) == SQLITE_OK &&
        sqlite3_bind_text(q, 1, want.c_str(), int(want.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_DONE;
    sqlite3_finalize(q);
    sqlite3_close(db);
    Check(ok, "self-test: cannot write " + reg.string());
    const std::string got = RegistryWalletPathIn(reg);
    Check(got == want, "self-test: registry path encoding differs: stored '" + want + "' read back '" + got + "'");
    Check(got != ExpectedRegistryWalletPath(fs::path("C:\\Users\\other\\datadir")),
          "self-test: different paths compare equal");
    fs::remove(reg);
    std::cout << "W4 self-test: backslash wallet path round-trips in the compared encoding\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
            return 1;
        }
        const fs::path fixture = argv[1];
        std::cout << "W4 config=" << kConfig << " fixture=" << fixture.string() << "\n";
        for (const auto& rel : DatabaseFiles())
            Check(fs::is_regular_file(fixture / rel), "fixture database missing: " + (fixture / rel).string());
        const std::string want_json = GeneratedText(ReadFile(fixture / "expected.json"));
        const std::string want_schema = GeneratedText(ReadFile(fixture / "schema.txt"));
        Check(json::parse(want_json).value("config", "") == kConfig, "fixture config does not match this build");

        TempDir temp;
        SelfTestWalletPathEncoding(temp.path);
        const fs::path root = (temp.path / "datadir").lexically_normal();
        fs::create_directories(root / "wallets");
        fs::create_directories(root / "blockchain");
        for (const auto& rel : DatabaseFiles()) {
            fs::copy_file(fixture / rel, root / rel);
            // A real user's wallet files are owner-only (the wallet creates
            // them under umask 0077); git checks fixtures out 0644.
            fs::permissions(root / rel, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        }
        for (const auto& dir : {root, root / "wallets", root / "blockchain"})
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);

        dinero::SelectParams(dinero::Chain::REGTEST);
        bool ok = true;
        const auto before = SchemaDump(root);
        ok &= Same("schema.txt (before open)", want_schema, before);
        const auto tree_before = TreeDump(root);
        const auto rows_before = RowDump(root);
        const auto header_before = HeaderDump(root);

        const auto got_json = Canonical(Derive(root, SEP_WALLET_SCHEMA_PATH));
        ok &= Same("expected.json (after read-write open)", want_json, got_json);

        const auto after = SchemaDump(root);
        ok &= Same("schema.txt (after open)", want_schema, after);
        // A clean close leaves no -wal/-shm/-journal (SchemaDump refuses
        // them), so the file set must be exactly the copied one.
        ok &= Same("data-directory file set (before vs after open)", tree_before, TreeDump(root));
        ok &= Same("persisted rows (before vs after open)", rows_before, RowDump(root));
        ok &= Same("database header bytes 18-23,44-67 (before vs after open)", header_before, HeaderDump(root));
        // wallets.path is excluded from the row diff (it moves with the copy)
        // but its post-open value is fully determined: check it exactly.
        ok &= Same("registry wallets.path after open (expected <datadir>/wallets/wallet_sepfixture.db)",
                   ExpectedRegistryWalletPath(root), RegistryWalletPath(root));

        const auto derived = json::parse(got_json);
        std::cout << "W4 compared: " << DatabaseFiles().size() << " DBs, " << Lines(want_schema)
                  << " schema.txt lines, " << Lines(want_json) << " expected.json lines ("
                  << derived["wallet"]["addresses"].size() << " addresses, " << derived["wallet"]["utxos"].size()
                  << " wallet UTXOs, " << derived["utxo_index"]["utxos"].size() << " index UTXOs, "
                  << derived["descriptors"].size() << " descriptors; " << Lines(tree_before) << " datadir entries, "
                  << Lines(rows_before) << " row-dump lines, " << ExcludedColumns().size() << " excluded columns";
#ifdef SEP_FIXTURE_ORCHARD
        std::cout << ", Orchard snapshot revision " << derived["orchard"]["snapshot_revision"];
#endif
        std::cout << ")\n";
        std::cout << "W4 header fields:\n" << header_before;
        for (const auto& rel : DatabaseFiles())
            std::cout << "W4 " << rel << ": user_version=" << derived["sqlite"][rel]["user_version"]
                      << " journal_mode=" << derived["sqlite"][rel]["journal_mode"]
                      << " mmap_size=" << derived["sqlite"][rel]["mmap_size"] << "\n";
        if (!ok) {
            std::fprintf(stderr, "W4 FAIL: wallet files differ from the baseline fixture\n");
            return 1;
        }
        std::cout << "W4 OK: fixture opened read-write; contents, PRAGMAs, sqlite_master, rows, header fields, file set and wallet path unchanged\n";
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "W4 FAIL: %s\n", e.what());
        return 1;
    }
}
