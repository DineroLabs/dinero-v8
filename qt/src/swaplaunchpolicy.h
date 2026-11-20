#pragma once
// dinero-qt's "Enable swaps" switch: the setting it stores, the arguments the
// node is started with, and what the Swap tab says about it. Pure, so it is
// unit-tested (tests/test_swap_form_policy.cpp).
#include <QString>
#include <QStringList>

namespace SwapLaunchPolicy {

inline QString enabledKey() { return QStringLiteral("swap/enabled_v1"); }

// What both node launchers (main.cpp, mainwindow.cpp) add when the switch is on.
// swap.optional: a setting this node refuses leaves swaps off and the wallet's
// node still starts. Bitcoin is reached without Bitcoin Core; on mainnet swaps
// are a beta with small caps, which the user accepts when turning the switch on.
// The public watchtower run by Dinero Labs (design section 14.2). A buyer's
// node sends it pre-signed transactions only; it guards the swap while Dinero
// is closed.
inline QString publicTowerUrl() { return QStringLiteral("https://swaptower.dinerolabs.org"); }

// The Wallet page's own switch for the built-in Bitcoin wallet. It does not
// turn swaps on: with swaps off the node runs only its Bitcoin client and
// this wallet's Bitcoin account.
inline QString bitcoinWalletKey() { return QStringLiteral("btcwallet/enabled_v1"); }

inline QStringList daemonArgs(bool enabled, bool bitcoinWallet = false) {
    if (enabled) {  // swaps bring the Bitcoin client with them
        return {"--swap.enable=1", "--swap.optional=1", "--swap.btc_backend=light", "--swap.mainnet_beta=1",
                "--swap.tower_url=" + publicTowerUrl()};
    }
    if (bitcoinWallet) return {"--swap.btc_wallet=1", "--swap.optional=1"};
    return {};
}

// The node's own reason after "swaps are disabled: " when it refused the
// settings; empty when swaps were simply never asked for.
inline QString refusalReason(const QString& nodeMessage) {
    const QString prefix = "swaps are disabled: ";
    if (!nodeMessage.startsWith(prefix)) return {};
    const QString rest = nodeMessage.mid(prefix.size()).trimmed();
    return rest.startsWith("start dinerod with") ? QString() : rest;
}

// The reason in words for someone who never set a node option.
inline QString plainReason(const QString& reason) {
    if (reason.contains("built with DINERO_SWAP_LIGHT_BTC"))
        return "this copy of Dinero was built without them.";
    return reason;
}

enum class Node { Unknown, Off, On };  // no answer yet; swap.list refused; swap.list answered

struct SwitchView {
    QString text;
    QString button;
    bool turnsOn{true};       // what a click does
    bool buttonEnabled{true};
};

inline SwitchView switchView(bool wanted, Node node, const QString& reason, bool unfinishedSwaps) {
    SwitchView v;
    v.turnsOn = !wanted;
    v.button = wanted ? "Turn off swaps" : "Turn on swaps";
    if (node == Node::On) {
        if (!wanted) {
            v.text = "Swaps are on until Dinero is restarted.";
        } else if (unfinishedSwaps) {
            v.text = "Swaps are on. A swap is still open, so they stay on until it finishes or you cancel it.";
            v.buttonEnabled = false;
        } else {
            v.text = "Swaps are on.";
        }
        return v;
    }
    if (!wanted) {
        v.text = "Swaps are off. Turn them on to trade DIN for BTC directly with another person, with no exchange in "
                 "between.";
    } else if (node == Node::Off && !reason.isEmpty()) {
        v.text = "Swaps are switched on, but cannot start here: " + plainReason(reason);
    } else {
        v.text = "Swaps are switched on and start the next time Dinero starts: quit Dinero (File → Quit) and open it "
                 "again.";
    }
    return v;
}

// What the user agrees to when turning the switch on.
inline QString enableQuestion() {
    return "Swaps are a beta. Use small amounts only.\n\n"
           "While a swap is running, keep Dinero open and the wallet unlocked until it finishes. If you stop part "
           "way, your coins come back to you only after the swap's deadline.\n\n"
           "When you buy DIN, your node sends the swap's terms and pre-signed transactions to the Dinero Labs "
           "watchtower, which finishes the swap for you if Dinero is closed. It cannot take your coins.\n\n"
           "Swaps start the next time Dinero starts. Turn them on?";
}

}  // namespace SwapLaunchPolicy
