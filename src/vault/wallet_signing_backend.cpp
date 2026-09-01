// Copyright (c) 2026 Dinero Labs.

#include "vault/wallet_signing_backend.h"

#include "common/logger.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dinero::vault {

namespace {

std::string toHex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0xf]);
        out.push_back(digits[data[i] & 0xf]);
    }
    return out;
}

bool fromHex(const std::string& hex, uint8_t* out, size_t out_len) {
    if (hex.size() != out_len * 2) {
        return false;
    }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };
    for (size_t i = 0; i < out_len; ++i) {
        int hi = nib(hex[2 * i]);
        int lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

/// An all-zero txid is never a real one (it is a hash), so it is used as
/// the "attempt journalled, outcome unknown" marker.
bool isUnresolved(const std::array<uint8_t, 32>& txid) {
    for (uint8_t byte : txid) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

LedgerTimestamp nowNanos() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

WalletSigningBackend::WalletSigningBackend(BackendId id, SendCallback send,
                                           FloatCallback float_lookup,
                                           std::string idempotency_path)
    : id_(std::move(id)),
      send_(std::move(send)),
      float_lookup_(std::move(float_lookup)),
      idempotency_path_(std::move(idempotency_path)) {
    if (!send_) {
        throw std::invalid_argument("WalletSigningBackend: send callback required");
    }
    if (!float_lookup_) {
        throw std::invalid_argument("WalletSigningBackend: float_lookup callback required");
    }
    loadIdempotency();
}

UnaAmount WalletSigningBackend::availableFloat() {
    try {
        return float_lookup_();
    } catch (const std::exception& e) {
        dinero::g_logger.warn(std::string("[Vault] float lookup failed: ") + e.what());
        return 0;
    }
}

std::array<uint8_t, 32> WalletSigningBackend::signAndBroadcast(const UnsignedTx& tx) {
    std::lock_guard<std::mutex> lock(mu_);

    std::string req_hex = toHex(tx.request_id.data(), tx.request_id.size());
    auto it = cache_.find(req_hex);
    if (it != cache_.end()) {
        if (isUnresolved(it->second)) {
            // We journalled an attempt for this request and never recorded
            // an outcome — the process died, or the wallet call threw, in
            // the window where the broadcast may or may not have happened.
            // Re-sending would double-spend. Refuse and let an operator
            // reconcile against the chain.
            throw SigningBackendError(
                SigningBackendError::Kind::DUPLICATE_REQUEST,
                "WalletSigningBackend: request " + req_hex +
                    " was attempted but its outcome was never confirmed; "
                    "reconcile against the chain before retrying");
        }
        return it->second;
    }

    if (tx.outputs.empty()) {
        throw SigningBackendError(SigningBackendError::Kind::INTERNAL_ERROR,
                                  "WalletSigningBackend: empty outputs");
    }
    if (tx.outputs.size() > 1) {
        // The current vault.withdraw flow enqueues exactly one
        // output. Multi-output broadcasts would need batched
        // wallet support; surfacing as a clear error keeps the
        // fault localised.
        throw SigningBackendError(
            SigningBackendError::Kind::REJECTED_BY_POLICY,
            "WalletSigningBackend: only single-output withdrawals are supported");
    }

    const SignOutput& out = tx.outputs.front();

    // WRITE-AHEAD. The broadcast below is irreversible, so the journal has
    // to name this request before it can happen. Recording only the
    // outcome means a crash in the window leaves no trace at all, and the
    // retry double-spends.
    const std::array<uint8_t, 32> unresolved{};
    persistIdempotency(tx.request_id, unresolved);

    std::array<uint8_t, 32> txid{};
    try {
        txid = send_(out.script_pub_key, out.value, tx.fee_rate_hint, tx.audit_context);
    } catch (const SigningBackendError&) {
        cache_[req_hex] = unresolved;  // ambiguous: may or may not be on the wire
        throw;
    } catch (const std::exception& e) {
        cache_[req_hex] = unresolved;
        throw SigningBackendError(SigningBackendError::Kind::BROADCAST_FAILED,
                                  std::string("wallet send failed: ") + e.what());
    }

    // Assignment, not emplace: a poisoned in-memory marker must be
    // upgraded once the outcome is known.
    cache_[req_hex] = txid;
    persistIdempotency(tx.request_id, txid);
    return txid;
}

HealthReport WalletSigningBackend::healthcheck() {
    HealthReport report;
    report.backend = id_;
    report.timestamp = nowNanos();
    report.available_float = availableFloat();
    report.status = report.available_float == 0
                        ? BackendHealth{BackendDegraded{"available float is zero"}}
                        : BackendHealth{BackendHealthy{}};
    return report;
}

void WalletSigningBackend::loadIdempotency() {
    if (idempotency_path_.empty()) {
        return;
    }
    std::ifstream in(idempotency_path_);
    if (!in.is_open()) {
        return;
    }
    std::string line;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        // Format: <32-char request_id hex> <SP> <64-char txid hex>
        if (line.size() < 32 + 1 + 64) {
            continue;
        }
        std::string req_hex = line.substr(0, 32);
        std::string txid_hex = line.substr(33, 64);
        std::array<uint8_t, 32> txid{};
        if (!fromHex(txid_hex, txid.data(), txid.size())) {
            continue;
        }
        // Append-only journal, last line wins: an attempt (all-zero txid)
        // followed by an outcome resolves; an attempt with no outcome stays
        // unresolved and poisons that request against retry.
        cache_[req_hex] = txid;
        ++loaded;
    }
    size_t unresolved_count = 0;
    for (const auto& [req, txid] : cache_) {
        if (isUnresolved(txid)) {
            ++unresolved_count;
        }
    }
    if (loaded > 0) {
        dinero::g_logger.info(std::string("[Vault] idempotency cache loaded: ") +
                              std::to_string(loaded) + " entries");
    }
    if (unresolved_count > 0) {
        dinero::g_logger.warn(
            std::string("[Vault] ") + std::to_string(unresolved_count) +
            " withdrawal request(s) were broadcast-attempted with NO recorded outcome. "
            "Those coins may or may not be on-chain. Retries are refused until an "
            "operator reconciles them against the chain.");
    }
}

void WalletSigningBackend::persistIdempotency(const std::array<uint8_t, 16>& request_id,
                                              const std::array<uint8_t, 32>& txid) {
    if (idempotency_path_.empty()) {
        return;
    }
    try {
        std::filesystem::create_directories(
            std::filesystem::path(idempotency_path_).parent_path());
    } catch (...) {
        // best-effort
    }
    std::ofstream out(idempotency_path_, std::ios::app);
    if (!out.is_open()) {
        dinero::g_logger.warn("[Vault] could not open idempotency file for append: " +
                              idempotency_path_);
        return;
    }
    out << toHex(request_id.data(), request_id.size()) << " "
        << toHex(txid.data(), txid.size()) << "\n";
}

}  // namespace dinero::vault
