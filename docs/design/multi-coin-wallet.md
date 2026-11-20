# One wallet, several coins: Bitcoin first

Status: 2026-11-20. Bitcoin is built on a branch of its own (section 11); nothing of it is in a release. Owner's direction: dinero-qt should be a
one-stop wallet with a portfolio of everything the user holds, starting with a real Bitcoin wallet
and open to Zcash and whatever coins swaps add later.

## 1. What the user gets

- A **Portfolio** view: every coin this wallet holds, its balance, and what is on its way.
- A **Bitcoin wallet** inside dinero-qt: receive, balance, history, send. No Bitcoin Core, no
  exchange, no second app.
- **Swaps that need nothing pasted.** Selling DIN pays the BTC into this wallet by default. Buying
  DIN can be paid straight from this wallet's BTC. "Use another address" and "pay from an exchange"
  stay as choices.
- **One backup.** The recovery words the user already has restore every coin.

Not in scope: custody for anyone else, an exchange, price charts, coins that are not also offered
in swaps.

## 2. What exists today

| Piece | State |
|---|---|
| Keys for other coins from the Dinero seed | Per-swap Bitcoin and Zcash keys under `m/SWAP'/net'/…` (`swap_manager.h`). No coin has a wallet-level account yet. |
| The seed | BIP39 recovery words (`wallet/address.h`). |
| Bitcoin without Bitcoin Core | Built and rehearsed on testnet4 (`din-btc-swaps-without-bitcoin-core.md`, sections 13 to 15): own header chain, Merkle proofs, public Esplora servers, broadcast over the Bitcoin network. |
| Reading what an address received | `BtcDepositWatcher`: proven coins, confirmations, and an unproven view for recovery. One address at a time. |
| Building and signing Bitcoin transactions | P2WPKH spends for swap locks and sweeps (`btc_deposit.cpp`), largest-first selection, fee from the servers' median. |
| Zcash | Transparent transactions are built and signed for swaps. Everything else needs a Zebra full node. |

## 3. Rules the design holds to

1. **One seed, standard paths.** Each coin's keys sit where that coin's own wallets expect them
   (Bitcoin: BIP84, `m/84'/0'/0'`). A user can restore the same words in another Bitcoin wallet and
   find the coins. Nobody is locked in. Checked in the code (2026-11-19): a wallet created or
   restored from words stores exactly the standard BIP39 seed (`bip39::MnemonicToSeed`: PBKDF2-
   HMAC-SHA512, 2,048 rounds, salt "mnemonic" plus the optional BIP39 passphrase, 64 bytes, kept
   as `master_seed_`), and the master key is the standard BIP32 one (HMAC-SHA512 keyed "Bitcoin
   seed"). The BIP39 passphrase is its own optional field, not the wallet's encryption password.
   Two limits: a wallet made by the plain `wallet.create` RPC has a random 64-byte seed and no
   words, so its other coins can only be moved out by exporting keys; and a wallet restored with
   a BIP39 passphrase needs that passphrase in the other wallet too.
2. **Verified, or labelled.** A balance is shown as final only if the node proved it against a
   header chain it checked itself. Anything a server merely said is shown as "pending" or
   "unverified", per coin, next to the number.
3. **A coin is a module.** The wallet, the Portfolio view and swaps talk to every coin through the
   same small interface. Adding a coin is writing a module.
4. **The choice stays.** The built-in wallet is the default, never the only way.
5. **No surprise disclosures.** Public servers learn the addresses they are asked about, as they
   already do for swaps. Dollar values need a price server and are off unless the user turns them on.

## 4. The coin module

```cpp
struct CoinModule {
    std::string ticker, name;            // "BTC", "Bitcoin"
    Verification verification;           // Proven | ServerOnly | NeedsFullNode: shown in the UI
    virtual std::string NewReceiveAddress() = 0;       // next unused, from the seed
    virtual CoinBalance Balance() = 0;                 // confirmed, pending in, pending out
    virtual std::vector<CoinTx> History(size_t limit) = 0;
    virtual SendPreview Preview(const SendRequest&) = 0;   // fee, change, what arrives
    virtual std::string Send(const SendRequest&, const SendPreview&) = 0;  // txid
    virtual std::optional<std::string> BumpFee(const std::string& txid) = 0;
    virtual SwapLeg* Swap() = 0;         // null if the coin has no swaps
};
```

Dinero itself is presented through the same interface for the Portfolio view; its wallet code does
not change.

## 5. The Bitcoin wallet

**Keys.** BIP84 account 0: receive addresses `m/84'/0'/0'/0/i`, change `m/84'/0'/0'/1/i`
(testnet4: coin type 1). Native SegWit (`bc1q…`) only. Swap keys stay where they are.

**Finding the coins.** The watcher that follows one deposit address becomes one that follows the
wallet's address set: every address handed out, plus a look-ahead of 20 unused ones on each chain
(the usual gap limit), so a restore from the words finds everything. Each confirmed coin is proven
into the node's header chain exactly as swap deposits are.

**Balance.** Three numbers: confirmed (proven, at least one confirmation), arriving (seen but
unproven or unconfirmed), leaving (our own unconfirmed spends). Only the first is spendable by
default.

**History.** From the servers' per-script history, each confirmed entry proven; unconfirmed ones
marked. Kept in the wallet database so the list survives a server outage.

**Sending.** Preview first, always: amount, fee in sat and sat/vB, change, total. Fee from the
servers' estimates (the median, as swaps do) with three choices (slow, normal, fast) and a hard
ceiling per transaction. Transactions signal replaceability, so a stuck payment can have its fee
raised from the history list. Change goes to a fresh change address; dust change is added to the
fee and shown as such. "Send everything" is supported. Broadcast goes to Bitcoin peers and the
servers, as swap transactions do.

**What swaps gain.** Selling DIN: the payout defaults to a fresh wallet address. Buying DIN: the
lock is built from wallet coins, with no deposit step and no waiting for a deposit's
confirmations. The deposit address remains for paying from an exchange. Swap locks keep their
final (non-replaceable) form: the watchtower package depends on the lock's exact transaction.

**Interfaces.** `btc.getnewaddress`, `btc.getbalance`, `btc.listtransactions`, `btc.preview`,
`btc.send`, `btc.bumpfee`, and `coins.list` for the Portfolio view. All refuse on a locked wallet
where a key is needed.

## 5a. How a coin's addresses come to exist

Nobody creates a Bitcoin address as a separate act. Every address is calculated from the wallet's
seed along that coin's path, so the addresses of a new wallet and of a wallet made years ago are
equally there to be calculated, and nothing is migrated.

- **A new wallet:** after the words are written down and confirmed, the Portfolio lists the other
  coins as "not turned on". Creation may offer them as checkboxes.
- **An existing wallet:** the same switch, whenever the user wants it.
- **Turning a coin on** needs the wallet unlocked once: the node calculates that coin's account
  key from the seed and keeps the public half. From then on receive addresses and balances work
  while the wallet is locked; only sending needs an unlock.
- **Why a switch and not everything at once:** turning a coin on makes the node ask public servers
  about that coin's addresses, which should be the user's choice; and it is the moment to ask for a
  verified backup of the words.
- **A wallet with no words** (see rule 1) is told so, with the advice to make one that has them.

## 6. Portfolio view

One row per coin: name, balance, pending amounts, and how that balance is verified. Selecting a row
opens that coin's receive, history and send. A coin the node cannot serve (no module, or a module
whose needs are not met) is listed greyed with the reason, not hidden. Dollar values appear only if
the user turns them on.

## 7. Zcash and other coins

| Coin | First step | What it needs |
|---|---|---|
| Zcash, transparent (`t1…`) | Receive, balance, send | Either a Zebra node (as swaps need today: label "needs a Zcash node"), or its own header chain and proofs. Zcash's proof of work (Equihash) and its upgrade schedule make that a separate piece of work, to be designed on its own. |
| Zcash, shielded | Later | Finding shielded payments means scanning with viewing keys through a light server. A project of its own; the Orchard code in this repository is Dinero's, not Zcash's. |
| Bitcoin Cash | After Bitcoin | The cheapest to add: the wallet module is mostly Bitcoin's. It needs its own header rules (a different difficulty adjustment), its own servers, its own address form, and a swap design and rehearsal of its own. |
| Ethereum, Solana | When their swaps are built | Account chains: keys are easy, checking a balance without trusting a server is not. They would start as "server only" and say so. |

## 8. What can go wrong, and what the design does about it

| Risk | Answer |
|---|---|
| A bug sends coins to the wrong place or burns them as fee | Preview equals what is signed (built once, shown, then signed); fee ceiling; send tests against a real regtest node for every path; a beta cap on the amount per send until it has run on mainnet for a release. |
| A server lies about a balance | Confirmed coins are proven; a lie can only add "pending" entries or hide coins, never make the wallet spend what does not exist (the network refuses it). |
| A server hides coins | Two servers must answer on mainnet; a restore that finds less than expected can be pointed at another server. The coins are safe on chain regardless. |
| The user loses the words | Same as today, but more is at stake: the app asks for a verified backup before the first receive address of another coin is shown. |
| Address reuse and privacy | A fresh address per receive; the servers still learn the wallet's addresses. Stated in the app. A user's own server can be configured (`swap.btc_servers` already exists). |
| A locked or offline wallet during a payment | Sends are atomic from the user's view: nothing is broadcast until signed; an unbroadcast transaction is kept and retried or cancelled. |

## 9. Order of work

1. **Bitcoin wallet in the node**: keys, address-set watcher, balance, history, send, fee bump,
   the RPCs. Tests on regtest and testnet4. No UI yet.
2. **Swaps use it**: payout to the wallet by default; buying from wallet coins.
3. **dinero-qt**: Portfolio view, Bitcoin receive, history and send screens, the backup prompt.
4. **A mainnet beta** with a per-send cap, then the cap comes off.
5. **Zcash transparent**, on the design chosen in section 7.

Each step ships only with its tests, and none of it belongs in v8.2.4: it starts after the mainnet
swap rehearsal has passed.

## 10. Decisions for the owner

- **Release:** which release carries step 1 to 3 (proposed: the one after the swaps release).
- **Beta cap:** the most a single Bitcoin send may move while this is new (proposed: 0.01 BTC, the
  swap beta's ceiling).
- **Dollar values:** whether a price source is offered at all, and which.
- **Zcash:** start with "needs a Zcash node", or wait until it can run without one.

## 11. Built so far (2026-11-20)

On the branch `claude/portfolio-btc-balances`, on top of the swaps branch. Not in any release.

| Piece | Where | State |
|---|---|---|
| Bitcoin at the addresses swaps paid to (watch-only) | `swap.btcholdings`; Swap tab, "Bitcoin from your swaps" | Built, tested on two real daemons |
| Keys: the BIP84 account of the seed | `btc_wallet_keys` | Built; the vectors published with BIP84 |
| Receive addresses, balance, restore from the seed | `btc_wallet`; `btc.status`, `btc.enable`, `btc.getnewaddress`, `btc.getbalance` | Built; works while the wallet is locked |
| Paying: preview, then exactly that | `btc_wallet_send`; `btc.preview`, `btc.send` | Built; Bitcoin Core accepts the payments on regtest |
| Raising a stuck payment's fee | `btc.bumpfee` | Built; Bitcoin Core replaces the old payment |
| Transaction list | `btc.listtransactions` | Built. A send-all payment (no change) stays "not confirmed" in the list: its confirmations are read from its change |
| Swaps default to the wallet | `swap.offer`, `swap.deposit` | The wallet's current address stands in for a missing Bitcoin address. Paying a swap's deposit from the wallet is an ordinary payment to the deposit address |
| Portfolio tab | `portfoliowidget` | Built: coins, receive, send with review, list, fee raise |
| Public testnet4 | `tests/wallet/btc_wallet_testnet4_run.sh` | See the run's result in the pull request |

Differences from the sections above, decided while building:

- **Not a `CoinModule` interface yet** (section 4). There is one coin; the interface is drawn when the second one arrives and shows what the two really share.
- **Swap locks are not built from wallet coins** (section 5, "What swaps gain"). The lock must have a fixed, non-replaceable form that the watchtower package depends on; wallet payments are replaceable by design. Buying DIN from the wallet is therefore: pay the swap's deposit address from the wallet. It costs one more transaction and the deposit's two confirmations.
- **Balances are read when the user looks**, after the wallet has been turned on, not on a timer.
- **Mainnet:** turning the wallet on needs `swap.btc_wallet_mainnet=1` (off by default), and one payment sends at most 0.01 BTC.

Still open: dollar values (off, undecided), a history kept on disk across server outages, coin control, and every other coin.
