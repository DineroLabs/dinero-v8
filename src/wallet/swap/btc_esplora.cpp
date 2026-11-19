#include "wallet/swap/btc_esplora.h"

#include "crypto/sha256.h"

#include <json/json.h>

#include <cmath>
#include <memory>
#include <stdexcept>

namespace dinero::swap {
namespace {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::optional<std::vector<uint8_t>> Unhex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return out;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(2 * bytes.size());
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::optional<Json::Value> ParseJson(const std::string& text) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 32;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    try {
        if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors)) return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;  // jsoncpp throws on nesting beyond the stack limit
    }
    return value;
}

// A JSON number that is a whole value in [0, max].
std::optional<uint32_t> Uint(const Json::Value& v, uint32_t max) {
    if (!v.isUInt64()) return std::nullopt;
    const uint64_t n = v.asUInt64();
    if (n > max) return std::nullopt;
    return static_cast<uint32_t>(n);
}

constexpr uint32_t kMaxHeight = 100'000'000;

std::optional<EsploraTxStatus> ParseStatus(const Json::Value& v) {
    if (!v.isObject() || !v["confirmed"].isBool()) return std::nullopt;
    EsploraTxStatus s;
    s.confirmed = v["confirmed"].asBool();
    if (!s.confirmed) return s;
    const auto height = Uint(v["block_height"], kMaxHeight);
    const auto hash = v["block_hash"].isString() ? BtcHashFromHex(v["block_hash"].asString()) : std::nullopt;
    if (!height || !hash) return std::nullopt;
    s.height = *height;
    s.block_hash = *hash;
    return s;
}

bool Loopback(const std::string& url) {
    for (const char* prefix : {"http://127.0.0.1:", "http://127.0.0.1/", "http://localhost:", "http://localhost/"}) {
        if (url.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

EsploraServer::EsploraServer(std::string base_url, BtcHttp http) : base_(std::move(base_url)), http_(std::move(http)) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    const bool https = base_.rfind("https://", 0) == 0 && base_.size() > 8;
    if (!https && !Loopback(base_ + "/")) {
        throw std::invalid_argument("Bitcoin server address must be https://… (plain http only for this machine): " + base_);
    }
    for (char c : base_) {
        if (c <= ' ' || c > '~') throw std::invalid_argument("Bitcoin server address has an invalid character");
    }
    if (!http_.get || !http_.post) throw std::invalid_argument("no HTTP transport");
}

std::optional<std::string> EsploraServer::Get(const std::string& path) {
    const BtcHttpResponse r = http_.get(base_ + path);
    if (r.status >= 200 && r.status < 300) {
        if (r.body.size() > kEsploraMaxBody) {
            last_error_ = EsploraError::Malformed;
            return std::nullopt;
        }
        last_error_ = EsploraError::None;
        return r.body;
    }
    // 404 (and 400 for an id the server will not look up) mean "not here";
    // everything else is a server that cannot answer now.
    last_error_ = (r.status == 404 || r.status == 400) ? EsploraError::NotFound : EsploraError::Unreachable;
    return std::nullopt;
}

std::optional<uint32_t> EsploraServer::TipHeight() {
    const auto body = Get("/blocks/tip/height");
    if (!body) return std::nullopt;
    const std::string text = Trim(*body);
    if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos) {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    }
    return static_cast<uint32_t>(std::stoul(text));
}

std::optional<Bytes32> EsploraServer::BlockHashAt(uint32_t height) {
    const auto body = Get("/block-height/" + std::to_string(height));
    if (!body) return std::nullopt;
    const auto hash = BtcHashFromHex(Trim(*body));
    if (!hash) last_error_ = EsploraError::Malformed;
    return hash;
}

std::optional<BtcBlockHeader> EsploraServer::Header(const Bytes32& block_hash) {
    const auto body = Get("/block/" + BtcHashToHex(block_hash) + "/header");
    if (!body) return std::nullopt;
    const auto raw = Unhex(Trim(*body));
    const auto header = raw ? ParseBtcBlockHeader(*raw) : std::nullopt;
    // The header is identified by its hash: one that hashes to something else is not it.
    if (!header || BtcBlockHash(*header) != block_hash) {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    }
    return header;
}

std::optional<Bytes32> EsploraServer::BlockTxid(const Bytes32& block_hash, uint32_t index) {
    const auto body = Get("/block/" + BtcHashToHex(block_hash) + "/txid/" + std::to_string(index));
    if (!body) return std::nullopt;
    const auto txid = BtcHashFromHex(Trim(*body));
    if (!txid) last_error_ = EsploraError::Malformed;
    return txid;
}

std::optional<std::vector<uint8_t>> EsploraServer::RawTx(const Bytes32& txid) {
    const auto body = Get("/tx/" + BtcHashToHex(txid) + "/hex");
    if (!body) return std::nullopt;
    auto raw = Unhex(Trim(*body));
    if (!raw || raw->empty() || raw->size() > kBtcMaxRawTxSize) {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    }
    return raw;
}

std::optional<EsploraTxStatus> EsploraServer::TxStatus(const Bytes32& txid) {
    const auto body = Get("/tx/" + BtcHashToHex(txid) + "/status");
    if (!body) return std::nullopt;
    const auto json = ParseJson(*body);
    const auto status = json ? ParseStatus(*json) : std::nullopt;
    if (!status) last_error_ = EsploraError::Malformed;
    return status;
}

std::optional<EsploraMerkle> EsploraServer::MerkleProof(const Bytes32& txid) {
    const auto body = Get("/tx/" + BtcHashToHex(txid) + "/merkle-proof");
    if (!body) return std::nullopt;
    const auto json = ParseJson(*body);
    auto malformed = [&]() -> std::optional<EsploraMerkle> {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    };
    if (!json || !json->isObject() || !(*json)["merkle"].isArray()) return malformed();
    const auto height = Uint((*json)["block_height"], kMaxHeight);
    const auto position = Uint((*json)["pos"], 0xffffffff);
    const Json::Value& merkle = (*json)["merkle"];
    if (!height || !position || merkle.size() > kBtcMaxMerkleDepth) return malformed();
    EsploraMerkle out;
    out.height = *height;
    out.proof.position = *position;
    for (const Json::Value& entry : merkle) {
        const auto hash = entry.isString() ? BtcHashFromHex(entry.asString()) : std::nullopt;
        if (!hash) return malformed();
        out.proof.siblings.push_back(*hash);
    }
    return out;
}

std::optional<std::vector<EsploraScriptTx>> EsploraServer::ScriptHistory(const std::vector<uint8_t>& script_pubkey) {
    std::vector<uint8_t> digest(32);
    crypto::CSHA256().Write(script_pubkey.data(), script_pubkey.size()).Finalize(digest.data());
    const auto body = Get("/scripthash/" + Hex(digest) + "/txs");
    if (!body) return std::nullopt;
    const auto json = ParseJson(*body);
    auto malformed = [&]() -> std::optional<std::vector<EsploraScriptTx>> {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    };
    if (!json || !json->isArray() || json->size() > kEsploraMaxListedTxs) return malformed();
    std::vector<EsploraScriptTx> out;
    for (const Json::Value& entry : *json) {
        if (!entry.isObject() || !entry["txid"].isString()) return malformed();
        const auto txid = BtcHashFromHex(entry["txid"].asString());
        const auto status = ParseStatus(entry["status"]);
        if (!txid || !status) return malformed();
        out.push_back({*txid, *status});
    }
    return out;
}

std::optional<EsploraOutspend> EsploraServer::Outspend(const Bytes32& txid, uint32_t vout) {
    const auto body = Get("/tx/" + BtcHashToHex(txid) + "/outspend/" + std::to_string(vout));
    if (!body) return std::nullopt;
    const auto json = ParseJson(*body);
    auto malformed = [&]() -> std::optional<EsploraOutspend> {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    };
    if (!json || !json->isObject() || !(*json)["spent"].isBool()) return malformed();
    EsploraOutspend out;
    out.spent = (*json)["spent"].asBool();
    if (!out.spent) return out;
    const auto spender = (*json)["txid"].isString() ? BtcHashFromHex((*json)["txid"].asString()) : std::nullopt;
    const auto vin = Uint((*json)["vin"], 0xffffffff);
    const auto status = ParseStatus((*json)["status"]);
    if (!spender || !vin || !status) return malformed();
    out.txid = *spender;
    out.vin = *vin;
    out.status = *status;
    return out;
}

EsploraBroadcast EsploraServer::Broadcast(const std::vector<uint8_t>& raw_tx) {
    EsploraBroadcast out;
    const auto tx = ParseBtcRawTx(raw_tx);
    if (!tx) {
        out.error = "not a transaction";
        return out;
    }
    const BtcHttpResponse r = http_.post(base_ + "/tx", Hex(raw_tx));
    if (r.status >= 200 && r.status < 300) {
        // The server answers with the id; it must be the one we computed.
        const auto echoed = BtcHashFromHex(Trim(r.body.substr(0, 200)));
        if (!echoed || *echoed != tx->txid) {
            last_error_ = EsploraError::Malformed;
            out.error = "server answered with another transaction id";
            return out;
        }
        last_error_ = EsploraError::None;
        out.accepted = true;
        out.txid = tx->txid;
        return out;
    }
    last_error_ = r.status == 0 || r.status >= 500 || r.status == 429 ? EsploraError::Unreachable : EsploraError::NotFound;
    if (r.status == 0) {
        out.error = "no answer";
        return out;
    }
    // The server's reason, for a log line: bounded and printable.
    for (char c : r.body.substr(0, 200)) out.error.push_back(c >= ' ' && c <= '~' ? c : ' ');
    out.error = "HTTP " + std::to_string(r.status) + ": " + Trim(out.error);
    return out;
}

std::optional<double> EsploraServer::FeeRate(uint32_t target_blocks) {
    const auto body = Get("/fee-estimates");
    if (!body) return std::nullopt;
    const auto json = ParseJson(*body);
    if (!json || !json->isObject() || json->size() > 1000) {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    }
    // The listed target closest to ours from below (as urgent or more urgent),
    // else the most relaxed one listed.
    std::optional<std::pair<uint32_t, double>> best, lowest;
    for (const std::string& key : json->getMemberNames()) {
        if (key.empty() || key.size() > 5 || key.find_first_not_of("0123456789") != std::string::npos) continue;
        const Json::Value& v = (*json)[key];
        if (!v.isNumeric()) continue;
        const double rate = v.asDouble();
        if (!std::isfinite(rate) || rate <= 0 || rate > 100'000) continue;
        const auto target = static_cast<uint32_t>(std::stoul(key));
        if (target == 0) continue;
        if (target <= target_blocks && (!best || target > best->first)) best = {target, rate};
        if (!lowest || target < lowest->first) lowest = {target, rate};
    }
    if (!best && !lowest) {
        last_error_ = EsploraError::Malformed;
        return std::nullopt;
    }
    return best ? best->second : lowest->second;
}

std::optional<BtcProvenTx> FetchProvenTx(EsploraServer& server, const BtcHeaderChain& chain, const Bytes32& txid,
                                         std::string& why) {
    auto fail = [&](const std::string& reason) -> std::optional<BtcProvenTx> {
        why = reason;
        return std::nullopt;
    };
    auto lost = [&](const char* what) {
        return fail(std::string(what) + (server.last_error() == EsploraError::NotFound    ? ": not found"
                                         : server.last_error() == EsploraError::Malformed ? ": malformed answer"
                                                                                          : ": server unreachable"));
    };
    BtcProvenTx out;
    auto raw = server.RawTx(txid);
    if (!raw) return lost("transaction");
    auto tx = ParseBtcRawTx(*raw);
    if (!tx) return fail("transaction: not a transaction");
    if (tx->txid != txid) return fail("transaction: the server sent a different transaction");

    const auto status = server.TxStatus(txid);
    if (!status) return lost("status");
    if (!status->confirmed) return fail("unconfirmed");
    // The block must be one our own chain holds, at the height the server says.
    const auto height = chain.HeightOf(status->block_hash);
    if (!height) return fail("its block is not on our header chain");
    if (*height != status->height) return fail("the server's block height does not match our chain");
    const auto header = chain.AtHeight(*height);

    const auto proof = server.MerkleProof(txid);
    if (!proof) return lost("proof");
    if (proof->height != *height) return fail("the proof is for another block");
    const auto coinbase_txid = server.BlockTxid(status->block_hash, 0);
    if (!coinbase_txid) return lost("coinbase id");
    const auto coinbase_raw = server.RawTx(*coinbase_txid);
    if (!coinbase_raw) return lost("coinbase");
    const auto coinbase_proof = server.MerkleProof(*coinbase_txid);
    if (!coinbase_proof) return lost("coinbase proof");

    if (const auto refused = BtcCheckTxInclusion({*raw, proof->proof, *coinbase_raw, coinbase_proof->proof}, *header)) {
        return fail(*refused);
    }
    out.raw = std::move(*raw);
    out.tx = std::move(*tx);
    out.height = *height;
    out.block_hash = status->block_hash;
    out.confirmations = chain.Confirmations(status->block_hash);
    return out;
}

}  // namespace dinero::swap
