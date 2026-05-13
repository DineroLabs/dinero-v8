// Copyright (c) 2026 Dinero Labs.
//
// JSON-line file persistence for the vault ledger.
//
// Format: one entry per line, sorted-keys JSON without trailing
// whitespace. Variant tag goes in field "kind"; remaining fields
// are concrete-shape-dependent.

#include "vault/ledger_store.h"

#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dinero::vault {

namespace {

// ----- minimal JSON helpers -----
//
// We avoid pulling in a full JSON library (the project's `Json` is
// daemon-side and tied to the RPC layer). The vault store's needs
// are tiny — sorted-keys integer/string emit + a hand-rolled
// tokeniser on the read path.

std::string escapeString(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::setw(4) << std::setfill('0') << std::hex
                        << static_cast<int>(c);
                } else {
                    oss << c;
                }
        }
    }
    oss << '"';
    return oss.str();
}

std::string hexEncode(const std::array<uint8_t, 32>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

bool hexDecode(const std::string& hex, std::array<uint8_t, 32>& out) {
    if (hex.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        try {
            out[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::string outpointJson(const OutpointId& op) {
    std::ostringstream oss;
    oss << "{\"txid\":" << escapeString(hexEncode(op.txid_raw)) << ",\"vout\":" << op.vout << "}";
    return oss.str();
}

std::string serializeEntry(const LedgerEntry& entry) {
    std::ostringstream oss;
    std::visit(
        [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, DepositObserved>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"depositObserved\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditOpened>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditOpened\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditSettled>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditSettled\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CreditReverted>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"creditReverted\"" << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalInitiated>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"backend\":" << escapeString(concrete.backend.raw)
                    << ",\"kind\":\"withdrawalInitiated\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalSettled>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"kind\":\"withdrawalSettled\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, WithdrawalReverted>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"at\":" << concrete.at << ",\"kind\":\"withdrawalReverted\""
                    << ",\"request\":" << outpointJson(concrete.request)
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, CompensatingDebit>) {
                oss << "{\"account\":" << escapeString(concrete.account.raw)
                    << ",\"amount\":" << concrete.amount << ",\"at\":" << concrete.at
                    << ",\"deposit\":" << outpointJson(concrete.deposit)
                    << ",\"kind\":\"compensatingDebit\""
                    << ",\"operatorLoss\":" << concrete.operatorLoss
                    << ",\"seq\":" << concrete.seq << "}";
            } else if constexpr (std::is_same_v<T, PolicyAdjustment>) {
                oss << "{";
                oss << "\"account\":";
                if (concrete.account.has_value()) {
                    oss << escapeString(concrete.account->raw);
                } else {
                    oss << "null";
                }
                oss << ",\"at\":" << concrete.at
                    << ",\"deltaOperatorFloat\":" << concrete.deltaOperatorFloat
                    << ",\"deltaUserBalance\":" << concrete.deltaUserBalance
                    << ",\"kind\":\"policyAdjustment\""
                    << ",\"note\":" << escapeString(concrete.note)
                    << ",\"seq\":" << concrete.seq << "}";
            }
        },
        entry);
    return oss.str();
}

// ----- minimal JSON parser (reads what serializeEntry produces) -----

struct Parser {
    const std::string& s;
    size_t i{0};

    explicit Parser(const std::string& str) : s{str} {}

    void skipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }
    void expect(char c) {
        skipWs();
        if (i >= s.size() || s[i] != c) {
            throw LedgerStoreError(std::string{"expected '"} + c + "'");
        }
        ++i;
    }
    bool match(char c) {
        skipWs();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::string readString() {
        skipWs();
        if (i >= s.size() || s[i] != '"') {
            throw LedgerStoreError("expected string");
        }
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char next = s[i + 1];
                if (next == '"') {
                    out += '"';
                } else if (next == '\\') {
                    out += '\\';
                } else if (next == 'n') {
                    out += '\n';
                } else if (next == 'r') {
                    out += '\r';
                } else if (next == 't') {
                    out += '\t';
                } else {
                    out += next;
                }
                i += 2;
            } else {
                out += s[i];
                ++i;
            }
        }
        if (i >= s.size()) {
            throw LedgerStoreError("unterminated string");
        }
        ++i;
        return out;
    }
    int64_t readSignedInt() {
        skipWs();
        size_t start = i;
        if (i < s.size() && s[i] == '-') {
            ++i;
        }
        while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) {
            ++i;
        }
        return std::stoll(s.substr(start, i - start));
    }
    uint64_t readUnsignedInt() {
        skipWs();
        size_t start = i;
        while (i < s.size() && (s[i] >= '0' && s[i] <= '9')) {
            ++i;
        }
        return std::stoull(s.substr(start, i - start));
    }
    bool peekNull() {
        skipWs();
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return true;
        }
        return false;
    }
};

OutpointId parseOutpoint(const std::string& json) {
    Parser p{json};
    p.expect('{');
    OutpointId op;
    bool first = true;
    while (true) {
        if (p.match('}')) {
            break;
        }
        if (!first) {
            p.expect(',');
        }
        first = false;
        std::string key = p.readString();
        p.expect(':');
        if (key == "txid") {
            std::string hex = p.readString();
            if (!hexDecode(hex, op.txid_raw)) {
                throw LedgerStoreError("bad txid hex");
            }
        } else if (key == "vout") {
            op.vout = static_cast<uint32_t>(p.readUnsignedInt());
        } else {
            throw LedgerStoreError("unknown outpoint key: " + key);
        }
    }
    return op;
}

LedgerEntry parseEntry(const std::string& line) {
    // Find "kind":"…" pair; do a two-pass since variant choice
    // depends on it.
    size_t kind_pos = line.find("\"kind\":\"");
    if (kind_pos == std::string::npos) {
        throw LedgerStoreError("missing kind field");
    }
    size_t kind_start = kind_pos + std::string("\"kind\":\"").size();
    size_t kind_end = line.find('"', kind_start);
    if (kind_end == std::string::npos) {
        throw LedgerStoreError("malformed kind field");
    }
    std::string kind = line.substr(kind_start, kind_end - kind_start);

    Parser p{line};
    p.expect('{');
    // Common fields
    LedgerSeq seq = 0;
    LedgerTimestamp at = 0;
    AccountId account;
    bool account_set = false;
    OutpointId deposit_or_request;
    UnaAmount amount = 0;
    BackendId backend;
    UnaAmount operator_loss = 0;
    std::string note;
    int64_t delta_user = 0;
    int64_t delta_op = 0;
    bool first = true;
    while (true) {
        if (p.match('}')) {
            break;
        }
        if (!first) {
            p.expect(',');
        }
        first = false;
        std::string key = p.readString();
        p.expect(':');
        if (key == "kind") {
            (void)p.readString();
        } else if (key == "seq") {
            seq = p.readUnsignedInt();
        } else if (key == "at") {
            at = static_cast<LedgerTimestamp>(p.readSignedInt());
        } else if (key == "account") {
            if (p.peekNull()) {
                account_set = false;
            } else {
                account = AccountId{p.readString()};
                account_set = true;
            }
        } else if (key == "deposit" || key == "request") {
            std::string sub = "{";
            int depth = 1;
            p.expect('{');
            while (p.i < p.s.size() && depth > 0) {
                char c = p.s[p.i];
                sub += c;
                if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                }
                ++p.i;
            }
            deposit_or_request = parseOutpoint(sub);
        } else if (key == "amount") {
            amount = p.readUnsignedInt();
        } else if (key == "backend") {
            backend = BackendId{p.readString()};
        } else if (key == "operatorLoss") {
            operator_loss = p.readUnsignedInt();
        } else if (key == "note") {
            note = p.readString();
        } else if (key == "deltaUserBalance") {
            delta_user = p.readSignedInt();
        } else if (key == "deltaOperatorFloat") {
            delta_op = p.readSignedInt();
        } else {
            throw LedgerStoreError("unknown field: " + key);
        }
    }

    if (kind == "depositObserved") {
        return DepositObserved{seq, at, account, deposit_or_request, amount};
    }
    if (kind == "creditOpened") {
        return CreditOpened{seq, at, account, deposit_or_request, amount};
    }
    if (kind == "creditSettled") {
        return CreditSettled{seq, at, account, deposit_or_request};
    }
    if (kind == "creditReverted") {
        return CreditReverted{seq, at, account, deposit_or_request};
    }
    if (kind == "withdrawalInitiated") {
        return WithdrawalInitiated{seq, at, account, deposit_or_request, amount, backend};
    }
    if (kind == "withdrawalSettled") {
        return WithdrawalSettled{seq, at, account, deposit_or_request};
    }
    if (kind == "withdrawalReverted") {
        return WithdrawalReverted{seq, at, account, deposit_or_request};
    }
    if (kind == "compensatingDebit") {
        return CompensatingDebit{seq, at, account, deposit_or_request, amount, operator_loss};
    }
    if (kind == "policyAdjustment") {
        std::optional<AccountId> maybe_account;
        if (account_set) {
            maybe_account = account;
        }
        return PolicyAdjustment{seq, at, maybe_account, note, delta_user, delta_op};
    }
    throw LedgerStoreError("unknown kind: " + kind);
}

}  // namespace

FileLedgerStore::FileLedgerStore(std::string path) : path_{std::move(path)} {
    out_.open(path_, std::ios::app | std::ios::binary);
    if (!out_.is_open()) {
        throw LedgerStoreError("failed to open ledger store: " + path_);
    }
}

void FileLedgerStore::append(const LedgerEntry& entry) {
    std::lock_guard<std::mutex> lock(mu_);
    out_ << serializeEntry(entry) << "\n";
    out_.flush();
    if (!out_.good()) {
        throw LedgerStoreError("ledger store write failed: " + path_);
    }
}

std::vector<LedgerEntry> FileLedgerStore::loadAll() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LedgerEntry> out;
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return out;  // empty / not-yet-created file is fine
    }
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) {
            continue;
        }
        try {
            out.push_back(parseEntry(line));
        } catch (const LedgerStoreError& e) {
            throw LedgerStoreError("line " + std::to_string(lineno) + ": " + e.what());
        }
    }
    return out;
}

void FileLedgerStore::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    out_.flush();
}

}  // namespace dinero::vault
