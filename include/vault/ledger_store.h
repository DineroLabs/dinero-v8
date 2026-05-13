// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — persistence layer for the ledger.
//
// Append-only file-backed store. Each entry is serialised as one
// JSON line in a deterministic key order; on startup the daemon
// replays the file through the in-memory Ledger to recover state.
//
// Why JSON-lines and not LevelDB:
//   - Vault throughput is bounded by chain block rate, so write
//     volume is tiny compared to chainstate.
//   - Operator-side audit / forensics is much easier with a plain-
//     text log than a LevelDB binary.
//   - The schema-versioned format lets future C++ changes evolve
//     without a migration step (unrecognised fields preserved verbatim).
// LevelDB-backed variant slots in cleanly behind the same interface
// when throughput becomes a concern.

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

    /// Persist one entry. Throws on durable-write failure.
    virtual void append(const LedgerEntry& entry) = 0;

    /// Load every entry from the store, in seq order. Used on
    /// startup to replay state into a fresh in-memory Ledger.
    virtual std::vector<LedgerEntry> loadAll() = 0;

    /// Force an fsync so a clean shutdown / migration is durable.
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

/// File-backed JSON-line store. Each append() is a single line
/// flushed to disk.
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
