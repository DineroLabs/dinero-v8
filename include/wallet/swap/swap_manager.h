#pragma once
// Swap manager (milestone 6 of docs/design/din-btc-atomic-swaps-v1-plan.md):
// the wallet-side owner of all swaps, used by the swap.* RPCs and the daemon's
// background tick. Library code, independent of the daemon: keys come from a
// KeyDeriver, chains from DinRpc/BtcRpc.
//
// Keys are recoverable from the wallet seed. Under the dedicated purpose
// m/1398227280' ("SWAP"), per network:
//   store key   m/SWAP'/net'/0'          -> DeriveSwapStoreKey
//   swap i      DIN key m/SWAP'/net'/1'/i', BTC key m/SWAP'/net'/2'/i'
// Alice's secret is fresh random per offer and kept only in the encrypted
// files: a restore into an empty directory reuses indices (and keys), and a
// secret derived from them would repeat one already revealed on chain.
// An index is never reused: the next one is above both the stored counter and
// every index found on disk (a restored older backup cannot roll it back), and
// an index that already has a file is refused.
//
// Files in the swap directory (all 0600, directory 0700): swap-<i>.swap and
// offer-<i>.offer (Alice's offer awaiting Bob's accept, with its secret),
// both encrypted; a public <file>.id sidecar each, so a locked wallet can
// still list and cancel; next_index.

#include "wallet/swap/encrypted_store.h"
#include "wallet/swap/runner.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dinero::swap {

inline constexpr uint32_t kSwapKeyPurpose = 1398227280;  // "SWAP"

// Private key at a fully hardened path (components WITHOUT the hardened bit);
// std::nullopt when the wallet is locked or has no seed.
using KeyDeriver = std::function<std::optional<Bytes32>(const std::vector<uint32_t>& hardened_path)>;

struct SwapKeyMaterial {
    SwapKeys keys;
};

// Throw std::runtime_error("wallet locked ...") when the deriver returns nullopt.
Bytes32 SwapStoreKeyFromSeed(const KeyDeriver& derive, SwapNetwork network);
SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index);

// Witness-program payout script from a bech32/bech32m address with the given
// HRP: P2WPKH, P2WSH (v0, bech32) or P2TR (v1, bech32m). Throws std::invalid_argument.
std::vector<uint8_t> PayoutScriptFromAddress(const std::string& address, const std::string& hrp);

struct OfferRequest {
    uint64_t din_amount_una{};
    uint64_t btc_amount_sat{};
    std::string din_refund_address;  // Alice's own DIN address (refund payout)
    std::string btc_claim_address;   // where Alice receives the BTC
    uint32_t btc_lock_hours{48};     // T_btc = now + this
    uint32_t din_lock_hours{96};     // T_din = now + this (>= btc + 24)
    uint32_t expires_minutes{60};
    uint32_t n_din_confirmations{30};
    uint32_t n_btc_confirmations{3};
};

struct SwapSummary {
    std::string id;  // 16 hex chars of the offer id
    uint32_t index{};
    Role role{};
    SwapState state{};
    uint64_t din_amount_una{};
    uint64_t btc_amount_sat{};
    uint32_t t_btc_unix{};
    uint32_t t_din_unix{};
    bool tower_armed{false};
    bool pending_accept{false};  // Alice's offer, Bob has not answered yet
    bool wallet_locked{false};   // session not readable: wallet locked, swap paused
    std::vector<std::string> last_events;
};

struct SwapManagerConfig {
    std::string dir;
    SwapNetwork network{SwapNetwork::Mainnet};
    RunnerConfig runner;  // din_hrp / btc_hrp / fees / tower
    // Per-swap limits (0 = none). Enforced on offers made AND offers accepted.
    uint64_t max_btc_sat{0};
    uint64_t max_din_una{0};
    bool require_tower_for_bob{false};  // mainnet beta: Bob may not accept without a watchtower
};

class SwapManager {
public:
    SwapManager(SwapManagerConfig config, KeyDeriver derive, DinRpc din, BtcRpc btc);

    // Alice. Returns the offer text to send to Bob.
    std::string MakeOffer(const OfferRequest& request, uint32_t now_unix);

    // Bob pastes Alice's offer: returns {id, accept text to send back}.
    // Alice pastes Bob's accept: returns {id, nullopt}; the swap starts.
    struct AcceptResult {
        std::string id;
        std::optional<std::string> accept_text;
    };
    AcceptResult Accept(const std::string& text, const std::string& din_payout_address,
                        const std::string& btc_refund_address, uint32_t now_unix);

    std::vector<SwapSummary> List();
    SwapSummary Status(const std::string& id);
    // Only before this party has locked anything (state Accepted, or a pending
    // offer). Throws std::runtime_error otherwise.
    void Cancel(const std::string& id);

    // Recovery: broadcast this wallet's own refund for swap `id` now (see
    // SwapRunner::ForceRefund). Returns the txid; throws with the node's reason.
    std::string Refund(const std::string& id, uint32_t now_unix);

    // One tick of every unfinished swap. Returns false (and does nothing) when
    // the wallet is locked: swaps are paused, not failed.
    bool TickAll(uint32_t now_unix);

    // Optional: deliver tower packages (Bob) to this sink; enables the tower.
    void SetTowerSink(std::function<void(const std::string&)> sink);
    void SetTowerAck(std::function<bool(const std::string&)> ack);

private:
    struct Live {
        std::unique_ptr<EncryptedFileSwapStore> store;
        std::unique_ptr<RpcSwapChainIo> io;
        std::unique_ptr<SwapRunner> runner;
        std::vector<std::string> last_events;
    };
    std::string SwapPath(uint32_t index) const;
    std::string OfferPath(uint32_t index) const;
    uint32_t AllocateIndex();
    std::map<uint32_t, std::string> ScanDir(const std::string& ext) const;  // index -> path
    SwapSession LoadSession(uint32_t index, const Bytes32& store_key) const;
    SwapSummary Summarize(uint32_t index, const SwapSession& s, const std::vector<std::string>& events) const;
    std::optional<uint32_t> FindIndex(const std::string& id, bool& is_offer);
    void StartSession(uint32_t index, SwapSession session, const Bytes32& store_key);

    SwapManagerConfig config_;
    KeyDeriver derive_;
    DinRpc din_;
    BtcRpc btc_;
    std::function<void(const std::string&)> tower_sink_;
    std::function<bool(const std::string&)> tower_ack_;
    std::optional<Bytes32> store_key_;  // kept while the daemon runs: live swaps survive a relock
    Bytes32 StoreKey();                  // derive (and remember), or the remembered one; throws if neither
    std::mutex mu_;
    std::map<uint32_t, Live> live_;
};

std::string SwapId(const SwapOffer& offer);  // 16 hex chars of OfferId

// Mainnet beta (plan milestone 7): swaps on mainnet need an explicit opt-in
// (swap.mainnet_beta=1) and are capped per swap. A configured cap may be lower
// than the default, never above the compiled-in ceiling. Test networks: no caps.
inline constexpr uint64_t kBetaHardMaxBtcSat = 1'000'000;             // 0.01 BTC
inline constexpr uint64_t kBetaDefaultMaxBtcSat = 100'000;            // 0.001 BTC
inline constexpr uint64_t kBetaHardMaxDinUna = 100'000ULL * 100'000'000ULL;  // 100,000 DIN
inline constexpr uint64_t kBetaDefaultMaxDinUna = 10'000ULL * 100'000'000ULL;  // 10,000 DIN

struct BetaDecision {
    std::optional<std::string> refusal;  // set: do not enable swaps
    uint64_t max_btc_sat{0};
    uint64_t max_din_una{0};
};
// Fee settings (swap.din_fee_una, swap.din_fee_urgent_una, swap.btc_fee_sat):
// a description of what is wrong, or nullopt when they are usable.
std::optional<std::string> FeeConfigProblem(int64_t din_fee_una, int64_t din_fee_urgent_una, int64_t btc_fee_sat);

BetaDecision BetaPolicy(SwapNetwork network, bool mainnet_beta_opt_in, uint64_t configured_max_btc_sat,
                        uint64_t configured_max_din_una);

}  // namespace dinero::swap
