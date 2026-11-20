#pragma once
// The built-in Bitcoin wallet: what it remembers and what it reads
// (docs/design/multi-coin-wallet.md, section 5). Receiving and balances only;
// nothing here signs.
//
// One small file beside the swap records holds the account's public half and
// how many addresses have been handed out. No private key is in it: with the
// file alone nobody can spend, but they can see every address of the wallet.
//
// The balance is read address by address through the caller's reader (the
// light Bitcoin client: payments proven into the node's own header chain).
// Addresses are looked at up to the usual gap of 20 unused ones past the last
// one ever paid, on both branches, so a wallet restored from its words finds
// its coins without any record of them.

#include "wallet/swap/btc_wallet_keys.h"
#include "wallet/swap/btc_wallet_send.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dinero::swap {

struct BtcScriptCoin {
    Bytes32 txid{};  // wire byte order
    uint32_t vout{};
    uint64_t value_sat{};
};
struct BtcScriptActivity {
    uint64_t sat{};          // unspent, proven, at least one confirmation
    bool ever_paid{false};   // a proven payment exists, spent or not
    std::vector<BtcScriptCoin> coins;  // the unspent outputs making up `sat` (may be left empty by a reader that only counts)
    struct Payment {
        Bytes32 txid{};          // wire byte order
        uint64_t sat{};          // what it paid this script
        uint32_t confirmations{};
    };
    std::vector<Payment> payments;  // every proven payment to the script, spent or not (may be left empty likewise)
};

class BtcWallet {
public:
    // Throws std::runtime_error when no server answers: a balance is never guessed.
    using ScriptReader = std::function<BtcScriptActivity(const std::vector<uint8_t>& script_pubkey)>;

    static std::string FilePath(const std::string& dir);
    static bool Enabled(const std::string& dir);
    // Turns the wallet on by storing the account's public half. Doing it again
    // with the same account changes nothing. Throws std::runtime_error when a
    // different account is already stored: that file belongs to another seed.
    static void Enable(const std::string& dir, const BtcWalletAccount& account);

    // Throws std::runtime_error when the wallet is not turned on or its file is damaged.
    BtcWallet(std::string dir, std::string hrp);

    const BtcWalletAccount& account() const { return account_; }
    uint32_t receive_handed_out() const { return next_receive_; }

    // The next unused receive address, recorded on disk before it is returned.
    // Throws std::runtime_error once kBtcWalletGapLimit addresses in a row
    // have been handed out and none was ever paid: a restore from the words
    // looks no further than that, so coins sent to a later one would be missed.
    std::string NewReceiveAddress();
    // The receive address handed out last (index 0 is handed out if none was).
    std::string CurrentReceiveAddress();

    struct Balance {
        struct Row {
            uint32_t branch{}, index{};
            std::string address;
            uint64_t sat{};
        };
        uint64_t sat{};
        std::vector<Row> rows;       // the addresses holding coins
        uint32_t addresses_read{};
        std::vector<BtcWalletCoin> coins;  // what a payment can spend: every coin the reader listed
        struct Received {
            std::string txid_hex;  // as explorers show it
            uint32_t branch{}, index{};
            std::string address;
            uint64_t sat{};
            uint32_t confirmations{};
        };
        std::vector<Received> received;    // every proven payment to an address of the wallet
    };
    // Reads every address the wallet may own. Remembers what it learns: after
    // a restore, the next receive address is one past the last address paid.
    Balance ReadBalance(const ScriptReader& read);

    // Where a payment's change goes: the first change address never used.
    // Planning does not use it up; RecordSent does.
    uint32_t change_index() const { return paid_change_; }
    std::vector<uint8_t> ChangeScript() const { return BtcWalletScript(account_, 1, paid_change_); }

    // A payment this wallet broadcast: kept until its coins are seen spent, so
    // they are not chosen again in the moments before the servers know, and so
    // the bytes can be sent again. `used_change`: its change address is taken.
    struct Sent {
        std::string txid_hex;         // as explorers show it
        std::vector<uint8_t> raw;
        uint64_t amount_sat{}, fee_sat{}, change_sat{};
        std::vector<BtcWalletCoin> spent;  // its inputs, with whose key each is
    };
    void RecordSent(const Sent& sent, bool used_change);
    std::vector<Sent> SentPayments() const;
    // The network refused this payment outright: its coins are free again.
    void ForgetSent(const std::string& txid_hex);
    // Drops from `coins` what a recorded payment already spends.
    std::vector<BtcWalletCoin> WithoutSpent(std::vector<BtcWalletCoin> coins) const;

private:
    void Save() const;

    std::string dir_, hrp_;
    BtcWalletAccount account_;
    uint32_t next_receive_{0};   // receive addresses handed out: indexes below this
    uint32_t paid_receive_{0};   // one past the highest receive index ever paid
    uint32_t paid_change_{0};    // the same on the change branch
};

}  // namespace dinero::swap
