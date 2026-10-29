// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — persistence layer for the ledger.
//
// Existing append-only JSON-line ledger entry storage. loadAll() checks each
// complete record and terminal EOF before returning the complete vector. It
// preserves existing entry bytes and does not reconstruct a complete vault:
// deposit chain bindings, withdrawal payload/state, and durable vault ownership
// are not present in this legacy format. Runtime service restoration is a
// separate contract. Stream flushes below are not physical fsync guarantees.

#pragma once

#include "vault/ledger_entry.h"

#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dinero::vault {

class LedgerStoreError : public std::runtime_error {
   public:
    explicit LedgerStoreError(const std::string& message) : std::runtime_error(message) {}
};

/// Abstract persistence interface. Tests use a no-op store; daemon
/// production wiring uses FileLedgerStore.
class LedgerStore {
   public:
    LedgerStore() = default;
    LedgerStore(const LedgerStore&) = delete;
    LedgerStore& operator=(const LedgerStore&) = delete;
    LedgerStore(LedgerStore&&) = delete;
    LedgerStore& operator=(LedgerStore&&) = delete;
    virtual ~LedgerStore() = default;

    /// Append one entry. Concrete stores define their write durability.
    virtual void append(const LedgerEntry& entry) = 0;

    /// Read all present entries in strictly increasing sequence order.
    /// File reads throw on malformed, incomplete, or unavailable data; an
    /// empty vector is not a certificate of a new or complete vault owner.
    virtual std::vector<LedgerEntry> loadAll() = 0;

    /// Flush the store according to its concrete durability contract.
    virtual void flush() = 0;
};

/// In-memory store. Used by tests + by deployments that don't want
/// persistence (Stage 0 shadow soaks).
class InMemoryLedgerStore : public LedgerStore {
   public:
    void append(const LedgerEntry& entry) override { entries_.push_back(entry); }
    std::vector<LedgerEntry> loadAll() override { return entries_; }
    void flush() override {}

   private:
    std::vector<LedgerEntry> entries_;
};

/// File-backed JSON-line store. Construction explicitly opens for append
/// and may create a file. Each append flushes the C++ stream (not fsync).
/// Loading never rewrites malformed records or returns a decoded prefix.
/// External writers, deletion completeness, and backup rollback are outside
/// this in-process reader's contract.
class FileLedgerStore : public LedgerStore {
   public:
    explicit FileLedgerStore(std::string path);

    void append(const LedgerEntry& entry) override;
    std::vector<LedgerEntry> loadAll() override;
    void flush() override;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

   private:
    std::string path_;
    std::mutex mu_;
    std::ofstream out_;
};

}  // namespace dinero::vault
