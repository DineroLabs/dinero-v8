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
//               ZEC swaps: HTLC key m/SWAP'/net'/5'/i' (in place of the BTC
//               key), Bob's deposit key m/SWAP'/net'/6'/i'
// Alice's secret is fresh random per offer and kept only in the encrypted
// files: a restore into an empty directory reuses indices (and keys), and a
// secret derived from them would repeat one already revealed on chain.
// An index is never reused: the next one is above both the stored counter and
// every index found on disk (a restored older backup cannot roll it back), and
// an index that already has a file is refused.
//
// Files in the swap directory (all 0600, directory 0700): swap-<i>.swap and
// offer-<i>.offer (Alice's offer awaiting Bob's accept, with its secret),
// both encrypted; a public <file>.id sidecar each for listing and locating
// files. Cancellation authenticates the encrypted owner; a cached key may
// survive a relock, but an unopened locked wallet cannot delete an offer.
// next_index.

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

// All paths in one call MUST use one captured wallet seed. Components are
// fully hardened (WITHOUT the hardened bit). No chain/RPC callbacks may run
// under the seed owner. nullopt means unavailable, never a partial key set.
// New funding requires the existing persistent identity from that same lease.
// Existing claim/refund keys do not require or create an identity.
struct DerivedKeyBatch {
    std::vector<Bytes32> keys;
    // Read under the same lease as the seed; zero never authorizes funding.
    Bytes32 wallet_id{};
    DerivedKeyBatch() = default;
    DerivedKeyBatch(const DerivedKeyBatch&) = delete;
    DerivedKeyBatch& operator=(const DerivedKeyBatch&) = delete;
    DerivedKeyBatch(DerivedKeyBatch&&) noexcept;
    DerivedKeyBatch& operator=(DerivedKeyBatch&&) = delete;
    ~DerivedKeyBatch();
};
using KeyDeriver = std::function<std::optional<DerivedKeyBatch>(const std::vector<std::vector<uint32_t>>& hardened_paths, bool require_funding_identity)>;

struct SwapKeyMaterial {
    SwapKeys keys;
    Bytes32 wallet_id{};
    Bytes32 store_key{};  // derived in the SAME callback as these three keys
    ~SwapKeyMaterial();
};

// Throw std::runtime_error("wallet locked ...") when the deriver returns nullopt.
Bytes32 SwapStoreKeyFromSeed(const KeyDeriver& derive, SwapNetwork network);
SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index, bool require_funding_identity = false);
// The counter-chain key follows the chain: 2' for Bitcoin (as above), 5' for Zcash.
// One batch either way, so every key comes from the same wallet.
SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index, CounterChain chain,
                                 bool require_funding_identity = false);
// Bob's per-swap Zcash deposit key, m/SWAP'/net'/6'/i'.
Bytes32 ZecDepositKey(const KeyDeriver& derive, SwapNetwork network, uint32_t index);

// Zcash support (docs/design/din-zec-atomic-swaps.md), installed by
// swap_manager_zec.cpp in dinerod builds with DINERO_SWAP_ZEC. Without it the
// manager refuses Zcash offers; this file links no Zcash code.
struct ZecDepositStatus;
struct LightBtcSupport {
    // The Bitcoin side of one swap, from the node's light backend. deposit_key
    // is the buyer's per-swap deposit key (zero for a seller).
    std::function<std::unique_ptr<RpcSwapChainIo>(const SwapSession&, const RunnerConfig&, const Bytes32& deposit_key)>
        make_io;
    // The verified header chain's height; throws std::runtime_error while it is not synced.
    std::function<uint32_t()> tip_height;
    // Paying BTC in from a deposit address, to buy DIN (section 3.3 of the
    // design). All three, or none: without them this node only sells DIN.
    std::function<std::string(const Bytes32& deposit_key)> deposit_address;
    std::function<ZecDepositStatus(const Bytes32& deposit_key)> deposit_status;  // throws if no server answers
    // Sends every usable deposit coin to `to_script`; returns the txid, throws if none or refused.
    std::function<std::string(const Bytes32& deposit_key, const std::vector<uint8_t>& to_script)> sweep_deposit;
    // What sweep_deposit would send: unlike deposit_status this also counts
    // payments that cannot be proven (older than the header chain's
    // checkpoint), which is right for giving coins back and for nothing else.
    // Optional: without it deposit_status is used.
    std::function<uint64_t(const Bytes32& deposit_key)> recoverable_sat;
    uint64_t funding_fee_sat{3000};  // part of the deposit: the most the lock's network fee may be
    bool CanPay() const { return deposit_address && deposit_status && sweep_deposit; }
};
// A deposit's state, in the counter chain's smallest unit (zatoshi, or satoshi
// for a Bitcoin deposit: the field names are Zcash's, where deposits began).
struct ZecDepositStatus {
    uint64_t seen_zat{};       // paid to the deposit address, at least 1 confirmation
    uint64_t confirmed_zat{};  // deep enough to lock from (Zcash: 10 confirmations; Bitcoin: 2)
    uint32_t confirmations{};  // of the newest payment (0: none yet)
};
struct ZecSupport {
    // Chain access for a Zcash swap; deposit_key is zero for Alice.
    std::function<std::unique_ptr<RpcSwapChainIo>(const SwapSession&, const RunnerConfig&, const Bytes32& deposit_key)>
        make_io;
    std::function<std::string(const Bytes32& deposit_key)> deposit_address;
    std::function<ZecDepositStatus(const Bytes32& deposit_key)> deposit_status;  // throws if Zebra is unreachable
    // Sends every confirmed deposit coin to `to_script` (swap.cancel); returns the txid, throws if none or refused.
    std::function<std::string(const Bytes32& deposit_key, const std::vector<uint8_t>& to_script)> sweep_deposit;
    std::function<std::vector<uint8_t>(const std::string& address)> payout_script;  // a t-address of this network
    // Throws when the swap's window (T_zec + 24 h) reaches a Zcash network upgrade.
    std::function<void(const SwapOffer& offer, uint32_t now)> require_no_upgrade;
    std::function<uint32_t()> tip_height;  // throws if Zebra is unreachable
    uint64_t funding_fee_zat{20'000};      // part of the deposit: 2 x the conventional fee
    uint64_t spend_fee_zat{20'000};        // claim / refund, taken from the lock
};

// Witness-program payout script from a bech32/bech32m address with the given
// HRP: P2WPKH, P2WSH (v0, bech32) or P2TR (v1, bech32m). Throws std::invalid_argument.
std::vector<uint8_t> PayoutScriptFromAddress(const std::string& address, const std::string& hrp);
// A Bitcoin payout: the segwit forms above, or a legacy P2PKH / P2SH address
// (1… / 3… on mainnet, m… n… / 2… elsewhere) of the network `hrp` names.
// Exchanges and older wallets still hand those out. Throws std::invalid_argument.
std::vector<uint8_t> BtcPayoutScriptFromAddress(const std::string& address, const std::string& hrp);

struct OfferRequest {
    CounterChain chain{CounterChain::Bitcoin};  // Zcash: amounts in zatoshi, a t-address below
    uint64_t din_amount_una{};
    uint64_t btc_amount_sat{};
    // Explicit authorization for the wallet-funded DIN lock, independent of
    // the later claim/refund fees. A missing budget refuses a new offer.
    uint64_t din_funding_fee_rate_hint{};
    uint64_t din_funding_maximum_fee_una{};
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
    // Only a present, authenticated retained body gives a submission status.
    // false is an unknown/rejected submission outcome, never proof it was unsent.
    // true records an acknowledgement, not confirmation or continued admission.
    std::optional<bool> din_funding_submission_acknowledged;
    bool tower_armed{false};
    bool pending_accept{false};  // Alice's offer, Bob has not answered yet
    bool wallet_locked{false};   // session not readable: wallet locked, swap paused
    bool sweep_pending{false};   // Bob: done on chain, DIN not yet swept to the wallet
    std::vector<std::string> last_events;
    CounterChain chain{CounterChain::Bitcoin};
    // Bob's Zcash deposit, before he accepts (pending_deposit) and until his lock.
    bool pending_deposit{false};
    std::string deposit_address;
    std::string deposit_offer;  // the (public) offer text, so a wallet can resume the deposit after a restart
    uint64_t deposit_due_zat{};
    std::optional<ZecDepositStatus> deposit;  // nullopt: Zebra not reachable
};

// Bob's deposit for a Zcash offer: pay `address` from any Zcash wallet, then accept.
struct DepositInfo {
    std::string id;
    uint32_t index{};
    std::string address;
    uint64_t amount_due_zat{};  // the swap amount + the funding fee
    std::optional<ZecDepositStatus> status;
};

struct SwapManagerConfig {
    std::string dir;
    SwapNetwork network{SwapNetwork::Mainnet};
    RunnerConfig runner;  // din_hrp / btc_hrp / fees / tower
    // Per-swap limits (0 = none). Enforced on offers made AND offers accepted.
    uint64_t max_btc_sat{0};
    uint64_t max_din_una{0};
    bool require_tower_for_bob{false};  // mainnet beta: Bob may not accept without a watchtower
    bool btc_configured{true};          // false: no Bitcoin node (a Zcash-only node)
};

class SwapManager {
public:
    SwapManager(SwapManagerConfig config, KeyDeriver derive, DinRpc din, BtcRpc btc, DinFundingOwner funding = {});

    // Alice. Returns the offer text to send to Bob.
    std::string MakeOffer(const OfferRequest& request, uint32_t now_unix);

    // Bob pastes Alice's offer: returns {id, accept text to send back}.
    // Alice pastes Bob's accept: returns {id, nullopt}; the swap starts.
    struct AcceptResult {
        std::string id;
        std::optional<std::string> accept_text;
    };
    // `din_payout_address` is asked for only once every check has passed and
    // the swap is about to be recorded: a refused attempt must not take an
    // address from the wallet. (A buyer waiting for his deposit to confirm
    // retries Accept many times; taking one each time pushed his payout past
    // the address gap a restore from the recovery words looks through.)
    using PayoutAddressSource = std::function<std::string()>;
    AcceptResult Accept(const std::string& text, const PayoutAddressSource& din_payout_address,
                        const std::string& btc_refund_address, uint32_t now_unix);
    AcceptResult Accept(const std::string& text, const std::string& din_payout_address,
                        const std::string& btc_refund_address, uint32_t now_unix) {
        return Accept(text, PayoutAddressSource([&din_payout_address] { return din_payout_address; }),
                      btc_refund_address, now_unix);
    }

    std::vector<SwapSummary> List();
    SwapSummary Status(const std::string& id);
    // Only before this party has locked anything (state Accepted, or a pending
    // offer). Throws std::runtime_error otherwise.
    void Cancel(const std::string& id);

    // Recovery: broadcast this wallet's own refund for swap `id` now (see
    // SwapRunner::ForceRefund). Returns the txid; throws with the node's reason.
    std::string Refund(const std::string& id, uint32_t now_unix);

    // Bitcoin deposit addresses of this wallet that still hold coins, sent to
    // `btc_address`: a payment that arrived after its swap's lock was built or
    // after the swap ended, and everything after a restore from the seed
    // (the addresses are derived from the seed, so no swap record is needed).
    // Looks at every swap index used on this node and `extra_indexes` beyond
    // (a restored wallet has no records: the caller says how far to look).
    // Indexes whose deposit may still fund a swap are left alone and named in
    // `skipped`. Needs the light Bitcoin support with deposits.
    struct RecoveredDeposits {
        struct Sweep {
            uint32_t index{};
            uint64_t amount_sat{};  // what the address held (the network fee comes out of it)
            std::string txid;
        };
        std::vector<Sweep> sweeps;
        std::vector<std::string> skipped;   // "index 4: its swap has not locked yet", …
        uint32_t indexes_checked{};
    };
    RecoveredDeposits RecoverBtcDeposits(const std::string& btc_address, uint32_t extra_indexes);

    // One tick of every unfinished swap. Returns false (and does nothing) when
    // the wallet is locked: swaps are paused, not failed.
    bool TickAll(uint32_t now_unix);

    // Zcash support (dinerod built with DINERO_SWAP_ZEC). Not on mainnet: the
    // design starts with a testnet beta.
    void SetZecSupport(ZecSupport zec);
    // Bitcoin without Bitcoin Core (docs/design/din-btc-swaps-without-bitcoin-core.md),
    // installed by swap_light_btc_install.cpp in a dinerod built with
    // DINERO_SWAP_LIGHT_BTC. With it this node sells DIN for BTC; buying DIN
    // (which pays BTC) still needs Bitcoin Core and is refused.
    void SetLightBitcoin(LightBtcSupport light);
    bool LightBitcoin() const { return light_btc_.has_value(); }
    bool SupportsZec() const;
    bool SupportsBtc() const { return config_.btc_configured; }
    // Bob pastes a Zcash offer: allocates its swap index and deposit address
    // (kept across restarts). Repeating it returns the same deposit.
    DepositInfo PrepareDeposit(const std::string& offer_text, const std::string& zec_refund_address, uint32_t now_unix);

    // Optional: deliver tower packages (Bob) to this sink; enables the tower.
    void SetTowerSink(std::function<void(const std::string&)> sink);
    void SetTowerAck(std::function<bool(const std::string& id, const std::string& package_hash)> ack);

private:
    struct Live {
        std::unique_ptr<EncryptedFileSwapStore> store;
        std::unique_ptr<RpcSwapChainIo> io;
        std::unique_ptr<SwapRunner> runner;
        std::vector<std::string> last_events;
    };
    std::string SwapPath(uint32_t index) const;
    std::string OfferPath(uint32_t index) const;
    std::string DepositPath(uint32_t index) const;
    void RequireZec() const;
    DepositInfo DepositOf(uint32_t index, const SwapOffer& offer, const Bytes32& store_key);
    // What a chain's deposit flow needs, from ZecSupport or LightBtcSupport.
    struct DepositOps;
    DepositOps DepositOpsFor(CounterChain chain) const;  // throws when this node has no deposit flow for it
    bool PaysFromDeposit(const SwapSession& s) const;    // Bob, with a lock built from his deposit
    uint32_t AllocateIndex();
    std::map<uint32_t, std::string> ScanDir(const std::string& ext) const;  // index -> path
    SwapSession LoadSession(uint32_t index, const Bytes32& store_key) const;
    SwapSummary Summarize(uint32_t index, const SwapSession& s, const std::vector<std::string>& events) const;
    std::optional<uint32_t> FindIndex(const std::string& id, bool& is_offer);
    // DIN this wallet has promised but not yet sent: every pending offer, and
    // every swap where it sells DIN and has not seen its DIN lock yet. Each
    // counts its amount plus kDinFundingFeeAllowanceUna.
    uint64_t CommittedDinUna(const Bytes32& store_key);
    void RequireDinBalanceFor(uint64_t din_amount_una, const Bytes32& store_key);
    void StartSession(uint32_t index, SwapSession session, const Bytes32& store_key);

    SwapManagerConfig config_;
    KeyDeriver derive_;
    DinRpc din_;
    BtcRpc btc_;
    DinFundingOwner funding_;
    std::function<void(const std::string&)> tower_sink_;
    std::function<bool(const std::string&, const std::string&)> tower_ack_;
    std::optional<ZecSupport> zec_;
    std::optional<LightBtcSupport> light_btc_;
    std::optional<Bytes32> store_key_;  // kept while the daemon runs: live swaps survive a relock
    Bytes32 StoreKey();                  // binds the first key; relock retains it, a different key refuses
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
