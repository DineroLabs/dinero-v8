#include "wallet/swap/btc_wallet.h"

#include "wallet/swap/swap_crypto.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dinero::swap {
namespace {

const char* const kHeader = "dinero-btc-wallet 1";

std::string Checksum(const std::string& body) {
    uint8_t h[32];
    crypto::CSHA256().Write(reinterpret_cast<const uint8_t*>(body.data()), body.size()).Finalize(h);
    return detail::ToHex(std::vector<uint8_t>(h, h + 8));
}

std::string Body(const BtcWalletAccount& account, uint32_t next_receive, uint32_t paid_receive, uint32_t paid_change) {
    std::ostringstream out;
    out << kHeader << "\n"
        << "account " << detail::ToHex(EncodeBtcWalletAccount(account)) << "\n"
        << "receive " << next_receive << "\n"
        << "paid_receive " << paid_receive << "\n"
        << "paid_change " << paid_change << "\n";
    return out.str();
}

struct Stored {
    BtcWalletAccount account;
    uint32_t next_receive{}, paid_receive{}, paid_change{};
};

Stored Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("the Bitcoin wallet is not turned on");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const auto damaged = [&path]() -> std::runtime_error {
        return std::runtime_error("the Bitcoin wallet's file is damaged (" + path +
                                  "): turn the wallet on again to rebuild it from your seed");
    };
    const size_t at = text.rfind("check ");
    if (at == std::string::npos) throw damaged();
    const std::string body = text.substr(0, at);
    std::string check = text.substr(at + 6);
    while (!check.empty() && (check.back() == '\n' || check.back() == '\r')) check.pop_back();
    if (check != Checksum(body)) throw damaged();
    std::istringstream lines(body);
    std::string header, key, account_hex;
    Stored s;
    std::getline(lines, header);
    if (header != kHeader) throw damaged();
    if (!(lines >> key >> account_hex) || key != "account") throw damaged();
    if (!(lines >> key >> s.next_receive) || key != "receive") throw damaged();
    if (!(lines >> key >> s.paid_receive) || key != "paid_receive") throw damaged();
    if (!(lines >> key >> s.paid_change) || key != "paid_change") throw damaged();
    try {
        s.account = DecodeBtcWalletAccount(detail::FromHex(account_hex));
    } catch (const std::invalid_argument&) {
        throw damaged();
    }
    return s;
}

void Store(const std::string& path, const Stored& s) {
    const std::string body = Body(s.account, s.next_receive, s.paid_receive, s.paid_change);
    detail::WriteFileAtomically(path, body + "check " + Checksum(body) + "\n");
}

}  // namespace

std::string BtcWallet::FilePath(const std::string& dir) { return dir + "/btc-wallet.pub"; }

bool BtcWallet::Enabled(const std::string& dir) { return static_cast<bool>(std::ifstream(FilePath(dir))); }

void BtcWallet::Enable(const std::string& dir, const BtcWalletAccount& account) {
    const std::string path = FilePath(dir);
    if (Enabled(dir)) {
        std::optional<Stored> existing;
        try {
            existing = Load(path);
        } catch (const std::runtime_error&) {
            // Damaged: the account comes from the seed, so the file is rebuilt.
            // The counters start again; ReadBalance finds what was paid.
        }
        if (existing) {
            if (!(existing->account == account)) {
                throw std::runtime_error("a Bitcoin wallet of another seed is already stored here (" + path +
                                         "); move that file away first");
            }
            return;
        }
    }
    std::filesystem::create_directories(dir);
    Store(path, Stored{account, 0, 0, 0});
}

BtcWallet::BtcWallet(std::string dir, std::string hrp) : dir_(std::move(dir)), hrp_(std::move(hrp)) {
    const Stored s = Load(FilePath(dir_));
    account_ = s.account;
    next_receive_ = s.next_receive;
    paid_receive_ = s.paid_receive;
    paid_change_ = s.paid_change;
}

void BtcWallet::Save() const { Store(FilePath(dir_), Stored{account_, next_receive_, paid_receive_, paid_change_}); }

std::string BtcWallet::NewReceiveAddress() {
    if (next_receive_ >= paid_receive_ + kBtcWalletGapLimit) {
        throw std::runtime_error("the last " + std::to_string(kBtcWalletGapLimit) +
                                 " Bitcoin addresses of this wallet have not been paid yet: use one of them first "
                                 "(a wallet restored from its words looks no further)");
    }
    const uint32_t index = next_receive_;
    const std::string address = BtcWalletAddress(account_, 0, index, hrp_);
    ++next_receive_;
    try {
        Save();  // on disk before anyone is told the address
    } catch (...) {
        --next_receive_;
        throw;
    }
    return address;
}

std::string BtcWallet::CurrentReceiveAddress() {
    if (next_receive_ == 0) return NewReceiveAddress();
    return BtcWalletAddress(account_, 0, next_receive_ - 1, hrp_);
}

BtcWallet::Balance BtcWallet::ReadBalance(const ScriptReader& read) {
    Balance out;
    uint32_t paid[2] = {paid_receive_, paid_change_};
    for (uint32_t branch = 0; branch < 2; ++branch) {
        // Everything handed out, everything up to the last address paid, and
        // the gap beyond whichever is further.
        uint32_t known = branch == 0 ? std::max(next_receive_, paid[0]) : paid[1];
        for (uint32_t index = 0; index < known + kBtcWalletGapLimit; ++index) {
            const BtcScriptActivity activity = read(BtcWalletScript(account_, branch, index));
            ++out.addresses_read;
            if (activity.ever_paid || activity.sat > 0) {
                paid[branch] = std::max(paid[branch], index + 1);
                known = std::max(known, index + 1);
            }
            if (activity.sat > 0) {
                out.rows.push_back({branch, index, BtcWalletAddress(account_, branch, index, hrp_), activity.sat});
                out.sat += activity.sat;
            }
            for (const BtcScriptCoin& c : activity.coins) {
                BtcWalletCoin coin;
                coin.txid = c.txid;
                coin.vout = c.vout;
                coin.value_sat = c.value_sat;
                coin.branch = branch;
                coin.index = index;
                out.coins.push_back(coin);
            }
        }
    }
    const uint32_t next = std::max(next_receive_, paid[0]);  // never hand out an address that was paid
    if (paid[0] != paid_receive_ || paid[1] != paid_change_ || next != next_receive_) {
        paid_receive_ = paid[0];
        paid_change_ = paid[1];
        next_receive_ = next;
        Save();
    }
    return out;
}

namespace {

std::string SentPath(const std::string& dir) { return dir + "/btc-wallet.sent"; }

// One line per payment: txid amount fee change raw-hex outpoint,outpoint,…
std::string SentLine(const BtcWallet::Sent& p) {
    std::ostringstream out;
    out << p.txid_hex << ' ' << p.amount_sat << ' ' << p.fee_sat << ' ' << p.change_sat << ' ' << detail::ToHex(p.raw) << ' ';
    for (size_t i = 0; i < p.spent.size(); ++i) {
        out << (i ? "," : "") << detail::ToHex(std::vector<uint8_t>(p.spent[i].txid.begin(), p.spent[i].txid.end())) << ':'
            << p.spent[i].vout;
    }
    return out.str();
}

}  // namespace

std::vector<BtcWallet::Sent> BtcWallet::SentPayments() const {
    std::vector<Sent> out;
    std::ifstream in(SentPath(dir_));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Sent p;
        std::string raw_hex, outpoints;
        if (!(fields >> p.txid_hex >> p.amount_sat >> p.fee_sat >> p.change_sat >> raw_hex >> outpoints)) continue;
        try {
            p.raw = detail::FromHex(raw_hex);
            std::istringstream list(outpoints);
            std::string one;
            while (std::getline(list, one, ',')) {
                const size_t colon = one.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("outpoint");
                const auto txid = detail::FromHex(one.substr(0, colon));
                if (txid.size() != 32) throw std::invalid_argument("outpoint");
                BtcScriptCoin c;
                std::copy(txid.begin(), txid.end(), c.txid.begin());
                c.vout = static_cast<uint32_t>(std::stoul(one.substr(colon + 1)));
                p.spent.push_back(c);
            }
        } catch (const std::exception&) {
            continue;  // a torn last line: the payment itself is on the network or it is not
        }
        out.push_back(std::move(p));
    }
    return out;
}

void BtcWallet::RecordSent(const Sent& sent, bool used_change) {
    std::string text;
    for (const Sent& p : SentPayments()) text += SentLine(p) + "\n";
    text += SentLine(sent) + "\n";
    detail::WriteFileAtomically(SentPath(dir_), text);
    if (used_change) {
        ++paid_change_;  // its change address is taken, confirmed or not
        Save();
    }
}

void BtcWallet::ForgetSent(const std::string& txid_hex) {
    std::string text;
    for (const Sent& p : SentPayments()) {
        if (p.txid_hex != txid_hex) text += SentLine(p) + "\n";
    }
    detail::WriteFileAtomically(SentPath(dir_), text);
}

std::vector<BtcWalletCoin> BtcWallet::WithoutSpent(std::vector<BtcWalletCoin> coins) const {
    const auto sent = SentPayments();
    coins.erase(std::remove_if(coins.begin(), coins.end(),
                               [&sent](const BtcWalletCoin& c) {
                                   for (const Sent& p : sent) {
                                       for (const BtcScriptCoin& s : p.spent) {
                                           if (s.txid == c.txid && s.vout == c.vout) return true;
                                       }
                                   }
                                   return false;
                               }),
                coins.end());
    return coins;
}

}  // namespace dinero::swap
