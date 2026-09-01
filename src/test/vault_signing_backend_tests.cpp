// Copyright (c) 2026 Dinero Labs.
//
// Crash-safety gtests for WalletSigningBackend's idempotency journal.
//
// The invariant under test: a durable record of the ATTEMPT must exist
// before `send_()` broadcasts, because the broadcast is irreversible.
// Recording only the outcome means a crash in between leaves no trace,
// and a retry of that request_id double-spends.

#include <gtest/gtest.h>

#include "vault/signing_backend.h"
#include "vault/wallet_signing_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dinero::vault::testing {
namespace {

std::string tempIdempotencyPath() {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path tmp = std::filesystem::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "vault-idem-%lld-%llu.jsonl",
                  static_cast<long long>(now),
                  static_cast<unsigned long long>(counter.fetch_add(1)));
    return (tmp / buf).string();
}

std::array<uint8_t, 16> rid(uint8_t seed) {
    std::array<uint8_t, 16> r{};
    r.fill(seed);
    return r;
}

std::string ridHex(uint8_t seed) {
    std::string out;
    for (int i = 0; i < 16; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<int>(seed));
        out += buf;
    }
    return out;
}

UnsignedTx txFor(const std::array<uint8_t, 16>& request_id, UnaAmount value = 100) {
    UnsignedTx tx;
    tx.request_id = request_id;
    SignOutput out;
    out.value = value;
    out.script_pub_key = std::vector<uint8_t>{0x51, 0x20};
    out.script_pub_key.resize(34, 0xab);
    tx.outputs.push_back(out);
    return tx;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace

TEST(VaultSigningBackend, attemptIsDurableBeforeTheBroadcast) {
    // Asserting from INSIDE the send callback is the only way to prove
    // ordering: at the moment coins can move, the journal must already
    // name this request.
    const std::string path = tempIdempotencyPath();
    bool journal_had_attempt = false;
    WalletSigningBackend backend{
        BackendId{"test"},
        [&](const std::vector<uint8_t>&, UnaAmount, UnaAmount, const std::string&) {
            auto lines = readLines(path);
            for (const auto& line : lines) {
                if (line.rfind(ridHex(0x11), 0) == 0) {
                    journal_had_attempt = true;
                }
            }
            std::array<uint8_t, 32> txid{};
            txid.fill(0x77);
            return txid;
        },
        []() -> UnaAmount { return 1'000'000; },
        path};

    backend.signAndBroadcast(txFor(rid(0x11)));
    EXPECT_TRUE(journal_had_attempt)
        << "send_() ran before the attempt was journalled; a crash here loses the record";
    std::filesystem::remove(path);
}

TEST(VaultSigningBackend, completedRequestJournalsBothAttemptAndOutcome) {
    const std::string path = tempIdempotencyPath();
    WalletSigningBackend backend{
        BackendId{"test"},
        [](const std::vector<uint8_t>&, UnaAmount, UnaAmount, const std::string&) {
            std::array<uint8_t, 32> txid{};
            txid.fill(0x77);
            return txid;
        },
        []() -> UnaAmount { return 1'000'000; },
        path};

    auto txid = backend.signAndBroadcast(txFor(rid(0x12)));
    EXPECT_EQ(txid[0], 0x77);
    auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], ridHex(0x12) + " " + std::string(64, '0'));
    EXPECT_EQ(lines[1], ridHex(0x12) + " " + std::string(64, '7'));
    std::filesystem::remove(path);
}

TEST(VaultSigningBackend, requestWithUnknownOutcomeIsNeverRetried) {
    // Simulates the crash: the journal holds an attempt with no outcome.
    // Re-sending would double-spend, so the backend must refuse.
    const std::string path = tempIdempotencyPath();
    {
        std::ofstream out(path);
        out << ridHex(0x13) << " " << std::string(64, '0') << "\n";
    }
    bool sent = false;
    WalletSigningBackend backend{
        BackendId{"test"},
        [&](const std::vector<uint8_t>&, UnaAmount, UnaAmount, const std::string&) {
            sent = true;
            std::array<uint8_t, 32> txid{};
            return txid;
        },
        []() -> UnaAmount { return 1'000'000; },
        path};

    EXPECT_THROW({
        try {
            backend.signAndBroadcast(txFor(rid(0x13)));
        } catch (const SigningBackendError& e) {
            EXPECT_EQ(e.kind(), SigningBackendError::Kind::DUPLICATE_REQUEST);
            throw;
        }
    }, SigningBackendError);
    EXPECT_FALSE(sent) << "re-broadcast a request whose outcome was never confirmed";
    std::filesystem::remove(path);
}

TEST(VaultSigningBackend, resolvedAttemptReplaysAsTheCachedTxid) {
    // Attempt followed by an outcome: the request completed. A retry must
    // return the recorded txid rather than sending again.
    const std::string path = tempIdempotencyPath();
    {
        std::ofstream out(path);
        out << ridHex(0x14) << " " << std::string(64, '0') << "\n";
        out << ridHex(0x14) << " " << std::string(64, 'a') << "\n";
    }
    bool sent = false;
    WalletSigningBackend backend{
        BackendId{"test"},
        [&](const std::vector<uint8_t>&, UnaAmount, UnaAmount, const std::string&) {
            sent = true;
            std::array<uint8_t, 32> txid{};
            return txid;
        },
        []() -> UnaAmount { return 1'000'000; },
        path};

    auto txid = backend.signAndBroadcast(txFor(rid(0x14)));
    EXPECT_EQ(txid[0], 0xaa);
    EXPECT_FALSE(sent);
    std::filesystem::remove(path);
}

TEST(VaultSigningBackend, failedBroadcastLeavesTheAttemptUnresolved) {
    // A send that throws is exactly as ambiguous as a crash: the wallet
    // may or may not have put the tx on the wire. The attempt must stay
    // unresolved so the request is never silently retried.
    const std::string path = tempIdempotencyPath();
    WalletSigningBackend backend{
        BackendId{"test"},
        [](const std::vector<uint8_t>&, UnaAmount, UnaAmount, const std::string&)
            -> std::array<uint8_t, 32> {
            throw std::runtime_error("wallet exploded");
        },
        []() -> UnaAmount { return 1'000'000; },
        path};

    EXPECT_THROW(backend.signAndBroadcast(txFor(rid(0x15))), SigningBackendError);
    auto lines = readLines(path);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines[0], ridHex(0x15) + " " + std::string(64, '0'));
    std::filesystem::remove(path);
}

}  // namespace dinero::vault::testing
