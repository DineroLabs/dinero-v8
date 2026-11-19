# DIN <-> BTC swaps without Bitcoin Core ("light Bitcoin")

**Status:** design, revised 2026-11-18 after three independent reviews (security, code feasibility,
architecture; section 12). Steps 0 to 5 are built on `claude/swaps-light-base` (section 13): on
regtest a whole swap completes with no Bitcoin Core on either side. Not offered to users: off by
default at build time, refused on mainnet. Target: after v8.2.5.
**Owner:** Claude. **Depends on:** `claude/swaps-multichain` landing in `dinero-main` first (section 8).
**Builds on:** `din-btc-atomic-swaps.md`, `din-btc-atomic-swaps-v1-plan.md`, `din-zec-atomic-swaps.md`.

## 1. Problem

BTC swaps only switch on when `dinerod` is started with `swap.btc_rpc` pointing at the user's own
Bitcoin Core (`src/daemon/services/swap_service.cpp`). The Swap tab hides BTC without it. A user who
is not technical will never run Bitcoin Core, so today they cannot swap at all. Pasting an address
(made easier in `ada8de0ca`) only helps people who already got past that wall.

**Goal:** a user with nothing but dinero-qt and a phone wallet or exchange account can sell DIN for
BTC and buy DIN with BTC. Bitcoin Core stays supported and stays the stronger option.

**Non-goals:** holding a BTC balance in dinero-qt; a Dinero-run Bitcoin server; changing the swap
protocol, the offer/accept format, timelocks or the HTLC scripts.

## 2. What Bitcoin Core is used for today

| Use | Calls | Who needs it |
|---|---|---|
| Watch the BTC lock and its spend | `getblockchaininfo`, `getblockhash`, `getblock` (verbosity 2), `gettxspendingprevout`, `getrawtransaction` (`btc_watcher.cpp`) | both sides, and the tower |
| Scan start height | `getblockcount` (`swap_manager.cpp`) | both sides |
| Broadcast claim / refund | `sendrawtransaction` | both sides, and the tower |
| Pay BTC into the lock | `createrawtransaction`, `fundrawtransaction`, `signrawtransactionwithwallet`, `lockunspent`, `gettxout`, `sendtoaddress` (`runner.cpp`) | buyer of DIN only |

Only the last row needs a Bitcoin *wallet*. The seller of DIN needs a chain view and a way to
broadcast, nothing else: claims and refunds are already built and signed by the swap code with
seed-derived keys (`btc_tx.cpp`).

## 3. Approach

A second Bitcoin backend, chosen by config:

```
swap.btc_backend=core     # today's behaviour (default when swap.btc_rpc is set)
swap.btc_backend=light    # Bitcoin peers and public servers, verified
swap.btc_servers=https://mempool.space/api,https://blockstream.info/api,...
```

Light mode has three parts: where the data comes from, how it is checked, and how BTC is paid in.

### 3.1 Two sources: Bitcoin peers for truth, servers for lookup

**Bitcoin peers (P2P).** A minimal client that connects to a handful of peers found through the
DNS seeds and speaks only `version`/`verack`, `ping`/`pong`, `getheaders`/`headers`, and `tx`/`inv`
for sending. It does two jobs:

- **Header sync.** About 2,000 headers per message from several peers. This is where the header
  chain of 3.2 comes from. (Esplora serves one header per request, so catching up a six-month-old
  binary through servers would take tens of thousands of calls.)
- **Direct broadcast** of the node's own transactions, above all the claim that reveals the secret.
  No server sits between the node and the network for it.

It never downloads blocks and accepts no unsolicited data: every message has a hard size limit,
unknown commands are dropped, and a peer that sends anything malformed is disconnected. This is new
code parsing untrusted input, so it is fuzzed before it ships (section 9).

**Servers (Esplora HTTP API).** Esplora is the REST API served by mempool.space and
blockstream.info. Used for what peers cannot give cheaply: finding transactions by script, proofs,
unconfirmed spends, fee estimates. Endpoints:

- `GET /scripthash/:hash/txs`: transactions paying a script (the HTLC, the deposit address)
- `GET /tx/:txid/hex`, `GET /tx/:txid/merkle-proof`, `GET /tx/:txid/status`
- `GET /tx/:txid/outspend/:vout`: who spent the lock
- `POST /tx`: broadcast, in addition to P2P
- `GET /fee-estimates`
- `GET /block-height/:h`, `GET /block/:hash/header`: header fallback when port 8333 is blocked

At least two servers run by different operators must be reachable. One is queried each tick; the
others are asked whenever an answer would change what the node does, and on a slower cycle, so the
node does not trip rate limits near a deadline. Requests back off on HTTP 429.

**The daemon has no HTTPS client today.** `dinerod` does not link libcurl; the swap code reaches
Bitcoin Core through a plain-socket client with no TLS (`src/rpc_client.cpp`), and the tower links
only `dinero_rpc_client`. Step 1 therefore adds a small library (`dinero_swap_light`) around the
existing libcurl wrapper in `src/bridge/http_client.cpp`, linked into `dinerod`, the tower and each
test target that lists swap sources. Server URLs must be `https`. Certificate checking uses the
system store on macOS and Linux and a shipped CA bundle on Windows (the vendored curl is built on
OpenSSL with Schannel off); a test proves a bad certificate is refused.

Electrum-protocol servers are a possible later addition behind the same interface.

### 3.2 Header chain and proofs

The node keeps its own Bitcoin header chain in `<datadir>/swaps/btc_headers`:

- starts from a checkpoint compiled into the binary (height, hash, bits, time);
- each new header must link to the previous one, meet the target in its own `bits`, carry (mainnet)
  the `bits` the 2016-block retarget rule requires, and have a time above the median of the previous
  eleven and no more than 2 h ahead of the wall clock;
- the best chain is the one with the most work, whichever peer or server offered it.

Nothing a server says about a confirmed transaction is believed without a Merkle proof into a
header of that chain. The raw transaction is fetched and hashed locally; proofs deeper than the
block could hold, and 64-byte transactions (which can be passed off as inner tree nodes), are
refused. Confirmations are counted on the node's own chain. Median time past, which the engine's
safety rules use (`engine.h`: the 6 h claim cut-off, the 2 h MTP-lag limit), is computed from the
verified headers, not asked for.

**A stale chain stops everything.** If the best verified header is more than 2 h behind the wall
clock, the backend reports "not ok" and the engine does nothing, in every state. (Today the engine's
MTP-lag check only guards the buyer's funding step, `engine.cpp:195-199`; a stale chain would
otherwise hold back a refund for ever.)

**Unconfirmed spends are believed only when they prove themselves.** The engine acts on an
unconfirmed spend of the lock (`engine.cpp:111-123`, `:214-217`), and a server can invent one. The
light backend reports the lock as spent only if (a) the spending transaction's witness carries a
preimage that hashes to the payment hash, (b) the spend has a Merkle proof, or (c) it is the node's
own transaction. Anything else unconfirmed is ignored. With this rule one honest server is enough
to learn the secret and no lying server can stall a refund or a claim.

Testnet4 has minimum-difficulty blocks, so there the work check is weaker (link and stated-target
only). Regtest has no meaningful work; light mode on regtest is for tests only.

### 3.3 Paying BTC in without a Bitcoin wallet (buyer of DIN)

The shape of the Zcash deposit flow on `claude/swaps-multichain` (deposit before accept, QR code,
seen versus due, withdraw), rebuilt for Bitcoin. The Zcash code itself is Zcash-specific
(`zec_refund_address`, `deposit_*_zat`, a constant fee) and has never run on mainnet, so this is new
code following a proven pattern, not a port.

1. The node derives a per-swap deposit key from the wallet seed on a new branch (5 and 6 are taken
   by Zcash) and shows its `bc1q…` address and a `bitcoin:` QR code. The amount due is the swap
   amount plus a stated fee buffer.
2. The user pays it from any wallet or exchange. Several payments add up.
3. After 2 confirmations of the deposit the node builds the lock transaction itself: only the
   deposit outputs it needs (largest first, each above a minimum, so nobody can bloat the lock with
   dust), the exact HTLC amount out, change to the refund address. The fee is set at this moment
   from the median of the servers' estimates, limited to the buffer. All inputs are segwit and the
   node is the only signer, so the txid is fixed before anything is sent, which is what the tower
   package and the pinned watcher need.
4. **Withdraw works in every state.** The deposit address is watched for as long as the wallet
   exists. Any output the lock did not spend, including a payment that arrives after the offer
   expired (60 minutes by default; exchange withdrawals are often slower), after an abort, or after
   the swap finished, can be sent to the refund address. A lock that is still unconfirmed can be
   replaced by a withdrawal; that is safe because the seller acts only on a confirmed lock
   (`engine.cpp:86-87`).
5. **Restore.** Deposit keys are derived from the seed by swap index, so a wallet restored from its
   seed re-derives the addresses for the indexes in use and scans them. The amount due and the
   refund address are stored with the deposit record.

### 3.4 Where it plugs in

On `claude/swaps-multichain` the engine and the runner are chain-neutral: a `SwapChainIo` subclass
supplies the counter chain. Light Bitcoin is one more, `LightBtcChainIo`, overriding `ObserveBtc`,
`BroadcastBtc`, `FundBtc`, `PrepareFundBtc`, `PreparedFundingUnsent`, `ReleasePreparedFunding`,
`PinFunding`, `CounterFundsByPreparing` and `CounterBroadcastMayHaveLeft`. The Bitcoin defaults for
signing, fees and tower rungs stay. The offer format, engine and tower package format do not change.

The manager, the service and the tower tool are **not** neutral and do change:

- `SwapManager` builds `RpcSwapChainIo(din_, btc_, …)` directly for Bitcoin offers, in accept and in
  cancel, and asks Bitcoin Core for the scan start (`getblockcount`). These go through a backend
  factory.
- The deposit entry points only accept Zcash offers (`PrepareDeposit`, the `dinswap2o` prefix,
  `ZecSupport`). Bitcoin gets its own.
- `swap_service.cpp` refuses to start without `swap.btc_rpc`. It learns `swap.btc_backend`.
- The network guard compares Bitcoin Core's `chain` name. Light mode pins the genesis hash instead
  (as `GuardZecGenesis` does).
- `CounterBroadcastMayHaveLeft` matches Bitcoin Core's error text; light mode decides from whether
  any source has seen the transaction.
- `dinero-swap-tower` requires `--btc-rpc`. It gets the light backend (section 5).
- `btc_tx.cpp` has the BIP143 signature hash but no P2WPKH script, multi-input builder or change
  logic. Step 3 adds them.

## 4. Trust: what a bad server can do, and what stops it

Protocol order (`engine.h`): the DIN seller locks DIN first; the BTC seller locks BTC after
`n_din_confirmations`; the DIN seller claims the BTC, which reveals the secret; the BTC seller uses
the secret to claim the DIN.

| Attack by servers (possibly working with the other party) | Victim | Defence |
|---|---|---|
| Report a BTC lock that does not exist, so the seller of DIN reveals the secret for nothing | seller of DIN | The lock is only believed with a Merkle proof into the verified header chain at `n_btc_confirmations` deep. Faking it means mining that many real-difficulty blocks. |
| Accept the seller's BTC claim, learn the secret, never relay it | seller of DIN | The claim goes straight to Bitcoin peers as well as to every server, and is re-sent until it has a Merkle proof. The existing rule (claim only before `T_btc - 6 h`) leaves hours for this. |
| Invent an unconfirmed spend of the lock, to stall the buyer's refund or stop the seller re-sending her claim | both | Section 3.2: only self-proving or proven spends count. |
| Hide the seller's BTC claim from the buyer, so the buyer never learns the secret | buyer of DIN | The secret proves itself: it hashes to the payment hash, so one honest server is enough. Once the claim is in a block, the buyer also finds it without any server's help by fetching the proof for the lock's spend. |
| Hide or delay new blocks (stale tip) | both | Headers come from Bitcoin peers, not from the servers; a tip more than 2 h old stops the engine in every state (section 3.2). |
| Report wrong fee levels, so a transaction overpays or sticks | both | Median across servers, inside the existing hard limits; claims and refunds already climb a fee ladder. |
| Add dust to the deposit address to bloat the lock | buyer of DIN | Input selection in 3.3. |
| Serve a low-work fork | both | Most-work rule over verified headers; mainnet retarget rule; compiled checkpoint. |
| All servers unreachable | both | Same as Bitcoin Core being down today: the swap pauses, and the timelocks decide. The tab says so. |

**What is left.** If every Bitcoin peer the node reaches *and* every server are controlled by the
other party at the same time, the seller can lose the DIN (claim never relayed) and the buyer can
lose the BTC (claim hidden until the DIN deadline). That is an eclipse of the user's whole internet
connection; the same attacker could do the same to a pruned Bitcoin Core.

**Fee ceiling near a deadline.** Claim and refund fees are capped at 5% of the amount
(`runner.cpp:865-872`). On a small swap that is a low feerate, and a fee spike near `T_btc` can
leave the seller's claim unconfirmed after the secret is public. This exists today with Bitcoin
Core. Light mode makes small swaps more common, so the tab shows the feerate the cap allows next to
the current one before the user commits, and refuses to start when the cap is below it.

**Privacy.** The servers learn the deposit address, the HTLC address and the user's IP address.
Bitcoin Core users reveal none of this. The tab says so in light mode.

**Limits.** Light mode keeps the mainnet beta opt-in and the existing per-swap caps (`BetaPolicy`;
hard cap 0.01 BTC). No extra cap: faking one block costs far more than the cap, and the risks that
remain do not depend on the amount. BTC confirmations stay at the default of 3, which protects
against reorgs, not against servers.

## 5. The watchtower

On mainnet a buyer of DIN must have a running tower: `Accept` refuses without one
(`require_tower_for_bob`), and the tower tool itself needs Bitcoin Core.

**The rule stays.** The first draft proposed letting light-mode buyers swap without a tower, with a
warning. Review showed that is not safe: the seller's BTC claim never expires, and the buyer's
refund is sent only by his own running process (`engine.cpp:218-221`). A buyer who is offline, or
whose servers are unreachable, from `T_btc` until his refund confirms can lose the whole BTC amount
to a seller who claims late and then takes the DIN back. The original protocol design says the same
(`din-btc-atomic-swaps.md:140-142`).

So:

- **Selling DIN ships first** (steps 1 and 2). A seller needs no tower.
- **Buying DIN ships with a tower that runs in light mode** (step 5), so it needs no Bitcoin Core,
  only an always-on machine.
- **A public tower run by Dinero Labs** would remove the always-on machine. Towers hold only
  pre-signed transactions paying the user's own address, so the operator cannot take funds, but it
  is a service to run and monitor. Owner's decision, not needed before step 5.

## 6. Dinero-qt

- BTC appears in the Swap tab when either backend is available. In light mode a one-line notice
  reads: "Using public Bitcoin servers. Your own Bitcoin Core is safer and more private."
- Selling DIN: unchanged. Paste a Bitcoin address, as now.
- Buying DIN: after reviewing the offer the button reads "Get the deposit address" and the deposit
  box (QR code, address, seen / due, Withdraw deposit) appears, as for Zcash.
- Status lines name the real condition: "Waiting for your payment (0.0003 of 0.0012 BTC seen)",
  "Bitcoin servers disagree; waiting", "Cannot reach any Bitcoin server".
- When swaps are enabled and no Bitcoin Core is configured, light mode is used with no further
  setup. Swaps themselves stay off by default (`swap.enable`), and the mainnet opt-in stays explicit;
  whether dinero-qt gets an "Enable swaps" switch is a separate decision (section 10).
- The capability report and the two "start dinerod with … swap.btc_rpc" messages
  (`methods_swap.cpp`, `swapwidget.cpp`) are updated to name both backends.

## 7. Work, in the order it ships

Each step ends with tests that fail without it. New sources are added by hand to every test target
that lists swap sources, to `cmake/SwapTower.cmake`, and Qt tests to `qt/CMakeLists.txt`.

0. **Bring the Zcash branch onto current trunk** (section 8).
1. **Light client library.** HTTPS client and link plumbing, certificate handling per platform,
   Esplora parsing, P2P header sync and broadcast, header store, retarget and timestamp rules,
   Merkle proofs. Unit tests against a scripted server and a scripted peer: wrong proof, 64-byte
   transaction, low-work fork, stale tip, bad certificate, one honest server among liars. Fuzzing of
   the P2P and JSON parsers.
2. **Selling DIN in light mode.** Backend factory in the manager and service, config and
   capabilities, genesis guard, `LightBtcChainIo` for observing a confirmed lock and broadcasting.
   No deposit code and no mempool logic are needed for the seller.
   *After this step a seller of DIN can swap with no Bitcoin Core. First release.*
3. **Deposit funding.** Deposit key branch, deposit record and restart/restore scan, deposit
   watcher, P2WPKH lock builder, withdraw in every state, self-proving unconfirmed spends.
4. **Dinero-qt.** Backend notice, deposit box for BTC, status wording, fee-ceiling check, tests.
5. **Tower in light mode.** *After this step a buyer of DIN can swap with no Bitcoin Core.*
6. **Rehearsals** (section 9).

## 8. Why the Zcash branch lands first

`claude/swaps-multichain` is 35 commits ahead of and 1,157 behind `dinero-main`. It holds the
chain-neutral runner that steps 2 to 5 build on. Measured with `git merge-tree` against
`claude/v825-swap-v3`: 9 files conflict, in 16 hunks (`swap_manager.cpp` 6, `swapwidget.cpp` 2,
`swap_service.cpp` 2, `methods_swap.cpp`, `swap_manager.h`, `swap_service.h`, `http_rpc_server.cpp`
and two regtest scripts 1 each). The runner and the unit tests merge cleanly.

The text is the small part. The Zcash branch predates the DIN funding rework (`DinFundingOwner`,
`PrepareDinFunding`): after a clean merge the Zcash IO never receives a funding owner and
`RpcSwapChainIo::FundDin` throws, so a ZEC seller could not lock DIN. That wiring, and the Zcash
tests passing again, are part of step 0.

## 9. Proving it works

1. **Unit and fuzz:** steps 1 to 3 against a scripted server and peer, including every row of the
   table in section 4; fuzzing of everything that parses data from the network.
2. **Regtest, end to end:** a small test server that answers the Esplora API from a regtest
   `bitcoind` (same style as the Zebra harness), two `dinerod` nodes in light mode, a full swap in
   each direction, a refund in each direction, a withdraw, and a swap where one of three servers
   lies.
3. **Bitcoin testnet4:** a real swap against public testnet4 servers, extending the existing
   testnet4 rehearsal on the Zcash branch.
4. **Mainnet, smallest amounts:** one swap in each direction between two wallets of the owner, the
   BTC paid from an exchange withdrawal so the slow, batched path is exercised. Needs the owner's
   explicit go-ahead at the time, per transfer.

Light mode is not offered to users before step 4 passes.

## 10. Decisions

Settled after review (the owner delegated these):

1. **Tower:** the mainnet tower rule stays; selling ships first, buying ships with a light-mode
   tower (section 5).
2. **Sources:** Bitcoin peers for headers and broadcast, at least two server operators for lookups.
   No "three operators" gate; it was not shown that three independent mainnet operators exist.
3. **Limits:** existing caps and the 3-confirmation default, unchanged. Deposit depth 2.
4. **Direct P2P broadcast:** in the first version, together with P2P header sync.

Still the owner's:

- Whether Dinero Labs runs a public tower (before step 5 ships to non-technical buyers).
- Whether dinero-qt gets an "Enable swaps" switch, and when the mainnet beta opt-in is lifted.

## 11. Not verified

- Real rate limits of mempool.space and blockstream.info over hours. Measured once (2026-11-19,
  `swap_btc_http_tool deposit`): the deposit watcher on a mining pool's mainnet payout script made
  402 requests to the two servers in 59 seconds with none refused, and proved 23 unspent coins
  (68.03 BTC) into a chain synced over P2P; the unproven recovery view gave the same 23. A swap's
  own addresses need a handful of requests per round.
- That `outspend` reports unconfirmed spends on both servers (the regtest test server does).
- How many reachable Bitcoin peers a home connection gets when port 8333 is filtered.
- That static Linux release builds take libcurl cleanly.
- Whether the encrypted swap store tolerates new fields on downgrade (no format version was found
  in `encrypted_store.cpp`).
- That public testnet4 servers are reliable enough for the rehearsal.

## 12. Review record (2026-11-18)

Three independent read-only reviews of the first draft. What they changed:

- **Security:** no-tower buying rejected (section 5); self-proving rule for unconfirmed spends and
  the all-state stale-tip stop (3.2); withdraw in every state, fee at build time, input selection
  (3.3); header timestamp rules and Merkle hardening (3.2); the proposed lower cap dropped as
  buying no security; fee-ceiling and restore notes.
- **Feasibility:** the draft's claim that the daemon already had HTTPS was wrong (3.1); manager,
  service and tower are not chain-neutral (3.4); the deposit flow is a pattern, not reusable code
  (3.3); measured rebase size and the missing DIN funding wiring for Zcash (section 8); omitted
  config, key branch, persistence and CI work (section 7).
- **Architecture:** keep Esplora for lookups, add P2P for headers and broadcast (3.1); drop the
  three-operator gate; certificate handling per platform; seller-only first release needs no
  mempool logic.

A fourth review by a second model could not run (the Codex plugin's default model was refused by
the account).

## 13. What is built (2026-11-19)

On `claude/swaps-light-base`, behind the build option `DINERO_SWAP_LIGHT_BTC` (off by default) and
the config `swap.btc_backend=light`. Mainnet additionally needs `swap.btc_light_mainnet_rehearsal=1`.

| Piece | Files (`src/wallet/swap/`) | Proven by |
|---|---|---|
| Header chain: proof of work, retarget, time rules, most work, checkpoint, bounded forks | `btc_header_chain` | Real mainnet headers 0 to 2; 2,029 real mainnet headers across the retarget at block 973,728 and 2,029 real testnet4 headers (fixtures); Bitcoin Core's retarget vectors |
| Header file | `btc_header_store` | Restart, another network, damage |
| Any-transaction reader; Merkle proofs with the 64-byte and depth protections | `btc_raw_tx`, `btc_merkle_proof` | The real genesis transaction; trees of 1 to 33 transactions; 60,000 damaged inputs |
| Bitcoin P2P: headers, and transaction hand-over with a receipt | `btc_p2p`, `btc_p2p_client` | Real Bitcoin Core v31 on regtest; by hand, mainnet to the tip two servers reported |
| Server client; a transaction is returned only if proven | `btc_esplora` | A real mainnet transaction of block 973,000 with its real proof; fifteen single lies refused |
| HTTPS | `btc_http_curl` | Local servers: limits, no redirects, certificate and host name |
| Watcher: the engine's view | `btc_light_watcher` | A simulated Bitcoin with lying and hiding servers |
| Shared backend: sync (peers, then servers), broadcast | `btc_light_backend`, `btc_light_transport` | A scripted network |
| Runner and daemon wiring | `btc_light_chain_io`, `src/daemon/swap_light_btc_install.cpp` | `SwapLightBtcRegtest` |
| Deposit address, lock and sweep builders | `btc_deposit` | BIP173's address vector; real Bitcoin Core accepts the lock and the sweep (`SwapBtcDepositRegtest`) |
| Deposit watcher | `btc_light_deposit` | The simulated Bitcoin: depth, spent coins, invented payments and spends |
| Buying: manager deposit flow for Bitcoin, RPC, Swap tab deposit box | `swap_manager`, `methods_swap`, `qt/src/swapwidget` | `SwapLightBtcBuyRegtest`; manager and Swap tab tests |

`SwapLightBtcRegtest` is the end-to-end proof: Alice's `dinerod` has no Bitcoin RPC. It syncs
headers and broadcasts over Bitcoin P2P and asks two Esplora servers, one of which hides
everything. She sells DIN to Bob (an ordinary Bitcoin Core swap node), is killed and restarted
mid-swap, and is paid at a legacy address. Run with both servers hiding, or with every proof wrong,
she never sees the lock and never claims.

`SwapLightBtcBuyRegtest` is the same for buying, with neither `dinerod` having a Bitcoin RPC: Bob is
given a deposit address, pays it in two payments from an outside wallet, is refused at one
confirmation and accepted at two; his node builds and sends the lock; the swap completes; the excess
returns to his refund address. A first, part-paid deposit is cancelled and comes back.

Differences from the plan above, each found while building:

- **A transaction written to a peer is not delivered.** Bitcoin Core drops what a peer sent if the
  peer hangs up first. Each transaction is followed by a ping, and counts only when the pong
  arrives. (Section 3.1 did not have this.)
- **The header chain is shared across threads** (`swap.refund` runs on an RPC thread). Sync and
  broadcast work on a copy and merge back through the chain's checks; a watcher holds the lock for
  one observation.
- **An unknown transaction's status is `{"confirmed":false}` with HTTP 200** on both real servers,
  so existence is taken from the raw transaction, never from the status.
- **A Bitcoin deposit key is the wallet's 7' key** for the swap index (5' and 6' are Zcash's).
- **The fee buffer is `swap.btc_funding_fee_sat` (default 3,000 sat)**, added to the amount due; the
  lock pays the median of the servers' estimates, never below 1 sat/vB, and what the fee does not
  use returns with the change.
- **Withdraw in every state is `swap.recoverdeposits {btc_address, [extra_indexes]}`.** An unused
  deposit, and a swap that has locked nothing, are swept on cancel. Everything else (a payment that
  arrives after the lock was built or after the swap ended, and all of it after a restore from the
  seed) is sent to an address the user names. A deposit that may still fund a swap is left alone.
- **Giving coins back does not need a proof.** A payment in a block below the header chain's
  checkpoint cannot be proven (a stray deposit found a year later, with a newer release), yet it is
  the user's money; and a coin a server invents only makes a sweep the network refuses. Sweeps
  therefore take every unspent output the servers report whose transaction hashes right and pays the
  deposit script. Funding a swap still needs the proof.
- **The watchtower runs without Bitcoin Core** (`--btc-backend light`, step 5). A buyer on mainnet
  still needs one, on a machine that stays on.
- **Testnet4 tips disagree for blocks at a time.** Observed live: a peer and mempool.space were on
  different, equally valid branches of minimum-difficulty blocks three deep, and the server's own
  tip changed between two requests. A testnet rehearsal has to wait for more confirmations than
  mainnet would; a payment in a block our chain is not on is simply "not yet", never accepted.

Not built: a button for `swap.recoverdeposits` in the Swap tab, the fee-ceiling check in the Swap
tab (section 4), the testnet4 and mainnet rehearsals (step 6), a CA bundle for Windows, fuzz targets beyond the in-test random-damage
loops. The Windows socket code is written and has never been compiled.

## 14. The switch and the public watchtower (2026-11-19)

Two owner decisions after section 13: dinero-qt gets an "Enable swaps" switch, and Dinero Labs runs
a public watchtower (on the existing `dinerotx` server).

### 14.1 The switch

The Swap tab has one line saying where swaps stand and one button. The choice is a stored setting;
both node launchers add `--swap.enable=1 --swap.optional=1 --swap.btc_backend=light
--swap.mainnet_beta=1 --swap.tower_url=https://swaptower.dinerolabs.org` when it is on, and it takes effect the next time Dinero starts.

- **The wallet's node always starts.** `swap.optional=1` turns a refused swap setting into "swaps
  are off, and here is why" instead of a node that does not start. The reason reaches the tab
  through `swap.*` ("swaps are disabled: ...") and is shown in plain words.
- **Turning it on asks first**: swaps are a beta, small amounts, keep Dinero open until a swap ends.
- **It cannot be turned off while a swap is open.**
- **The mainnet gate is not the switch's to lift.** Swaps without Bitcoin Core stay refused on
  mainnet inside `dinerod` until the mainnet rehearsal has passed (section 7, step 6); the switch
  never passes the rehearsal flag. Until then a mainnet user who turns it on reads "not available
  on mainnet yet".
- The release `dinerod` must be built with `DINERO_SWAP_LIGHT_BTC=ON`, or the tab says this copy
  was built without swaps.

### 14.2 The public watchtower

The tower program is unchanged in what it does: it guards the packages in its inbox directory. Until
now only the buyer's own machine could put a package there. Two pieces let anyone do it over the
network:

- **`dinero-swap-tower-intake`**, the door: a small HTTP program in front of the inbox
  (`wallet/swap/tower_http.h`). `POST /v1/packages` takes a package, `GET /v1/armed/<hash>` says
  whether the tower has confirmed, within the last two minutes, that it is guarding it. It holds no
  key and reaches no chain. It listens on the local machine only; a web server in front ends TLS and
  limits how often one address may ask.
- **`swap.tower_url=https://HOST`** in `dinerod`: the buyer's node sends its package there and
  locks its BTC only once the tower confirms. It needs the HTTPS client, so the build with
  `DINERO_SWAP_LIGHT_BTC`. An address on another machine must be `https://`.

What strangers can and cannot do to it:

| Attempt | What stops it |
|---|---|
| Send something that is not a real package | Every rung and signature is verified before anything is kept. |
| Replace or block another buyer's package | A package is stored under its own SHA-256, never under the swap's id, and is never overwritten. Two packages naming the same swap sit side by side. |
| Read or guess files through the address | Only 64 lower-case hex digits ever reach a file name. |
| Fill the disk, or make the tower look up thousands of swaps | Size cap (256 KiB), a cap on packages held (20,000), a request limit at the web server, and `--expire-unfunded-after`: a package whose buyer never locked is dropped that long after its DIN deadline. A package costs chain lookups only while it is held. |
| Hold connections open | One request per connection, ten seconds for the whole exchange, a fixed number of connections. |

What the operator can and cannot do: the tower has only pre-signed transactions that pay the buyer's
own addresses, so it cannot take coins. It can fail to act, or say "armed" and not be; either is the
same as having no tower, and the buyer's own node still does everything while it is online. It
learns each swap's terms, addresses and the address the package came from. A buyer who wants none
of that runs their own tower with `swap.tower_inbox`, as before.

Proved on regtest (`SwapPublicTowerRegtest`): neither node has Bitcoin Core; the buyer's node arms
the tower over HTTP and locks only after its confirmation; the buyer goes offline with his BTC
locked; the tower, itself without Bitcoin Core, claims his DIN and sees it buried.

**Running since 2026-11-19: `https://swaptower.dinerolabs.org`** (mainnet), on `dinerotx`, set up
as in docs/swaps/PUBLIC_TOWER_RUNBOOK.md: the door behind nginx with a Let's Encrypt certificate, the
tower without Bitcoin Core against that machine's Dinero node, both under their own unprivileged
user. It was the first build of this code on Linux. The switch passes this address
(`--swap.tower_url`), and its question says so. No real swap has used it yet: that is the
rehearsal's job (section 7, step 6).
