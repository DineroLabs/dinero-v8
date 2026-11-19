#pragma once
// Client for public Bitcoin servers that speak the Esplora HTTP API
// (mempool.space, blockstream.info), for swaps without Bitcoin Core
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.1).
//
// Servers are used for what Bitcoin peers cannot give cheaply: finding
// transactions by script, proofs, who spent an output, fee estimates. They are
// not believed. EsploraServer only fetches and parses, strictly: every hash is
// 64 hex digits, every number is in range, every list is bounded, a reply over
// the size limit is refused. FetchProvenTx then proves a confirmed transaction
// into the node's own header chain (btc_header_chain.h, btc_merkle_proof.h);
// what does not prove is not returned.
//
// HTTP itself is injected (BtcHttp), so this file has no network code and can
// be tested against any reply a server could send.

#include "wallet/swap/btc_header_chain.h"
#include "wallet/swap/btc_merkle_proof.h"
#include "wallet/swap/btc_raw_tx.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

// A raw transaction arrives as hex: twice a block's size at most.
inline constexpr size_t kEsploraMaxBody = 2 * kBtcMaxRawTxSize + 1024;
inline constexpr size_t kEsploraMaxListedTxs = 100;

struct BtcHttpResponse {
    int status{0};  // HTTP status; 0: no answer (unreachable, timeout, TLS failure)
    std::string body;
};
struct BtcHttp {
    std::function<BtcHttpResponse(const std::string& url)> get;
    std::function<BtcHttpResponse(const std::string& url, const std::string& body)> post;
};

enum class EsploraError {
    None,
    Unreachable,  // no answer, or a server error (5xx, 429)
    NotFound,     // the server answered that it does not have it
    Malformed,    // an answer that is not what the API promises
};

struct EsploraTxStatus {
    bool confirmed{false};
    uint32_t height{0};
    Bytes32 block_hash{};  // wire byte order
};
struct EsploraOutspend {
    bool spent{false};
    Bytes32 txid{};  // the spending transaction
    uint32_t vin{0};
    EsploraTxStatus status;
};
struct EsploraScriptTx {
    Bytes32 txid{};
    EsploraTxStatus status;
};
struct EsploraMerkle {
    uint32_t height{0};
    BtcMerkleProof proof;
};
struct EsploraBroadcast {
    bool accepted{false};
    Bytes32 txid{};
    std::string error;  // the server's words, trimmed, when refused
};

class EsploraServer {
public:
    // `base_url` like "https://mempool.space/api". Plain http is accepted only
    // for this machine (127.0.0.1, localhost): tests and a user's own server.
    // Throws std::invalid_argument otherwise.
    EsploraServer(std::string base_url, BtcHttp http);

    const std::string& base_url() const { return base_; }
    EsploraError last_error() const { return last_error_; }

    std::optional<uint32_t> TipHeight();
    std::optional<Bytes32> BlockHashAt(uint32_t height);
    std::optional<BtcBlockHeader> Header(const Bytes32& block_hash);
    std::optional<Bytes32> BlockTxid(const Bytes32& block_hash, uint32_t index);
    std::optional<std::vector<uint8_t>> RawTx(const Bytes32& txid);
    std::optional<EsploraTxStatus> TxStatus(const Bytes32& txid);
    std::optional<EsploraMerkle> MerkleProof(const Bytes32& txid);
    // Transactions paying or spending `script_pubkey`, newest first (the
    // server's first page: enough for an address used by one swap).
    std::optional<std::vector<EsploraScriptTx>> ScriptHistory(const std::vector<uint8_t>& script_pubkey);
    std::optional<EsploraOutspend> Outspend(const Bytes32& txid, uint32_t vout);
    EsploraBroadcast Broadcast(const std::vector<uint8_t>& raw_tx);
    // sat/vB to confirm within `target_blocks`: the estimate for that target, or
    // for the nearest more urgent one the server lists.
    std::optional<double> FeeRate(uint32_t target_blocks);

private:
    std::optional<std::string> Get(const std::string& path);

    std::string base_;
    BtcHttp http_;
    EsploraError last_error_{EsploraError::None};
};

struct BtcProvenTx {
    std::vector<uint8_t> raw;
    BtcRawTx tx;
    uint32_t height{0};
    Bytes32 block_hash{};
    uint32_t confirmations{0};  // on the node's own chain, at the time of the call
};

// Fetches `txid` and proves it into `chain`. nullopt, with the reason in
// `why`, if the server is unreachable, the transaction is unknown or
// unconfirmed, its block is not on the node's best chain, or anything the
// server supplied fails to prove.
std::optional<BtcProvenTx> FetchProvenTx(EsploraServer& server, const BtcHeaderChain& chain, const Bytes32& txid,
                                         std::string& why);

}  // namespace dinero::swap
