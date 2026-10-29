# DIN ↔ ZEC Atomic Swaps

Status: **design, owner decisions recorded 2026-10-28.** No consensus change. Milestone 3 (the Zcash transaction FFI) is built and tested on the swaps branch; nothing else is built yet. Target release: v8.2.1.

DIN↔ZEC v1 is the DIN↔BTC swap with a Zcash transparent P2SH hash-lock in place of the Bitcoin P2WSH one. The Dinero side is unchanged, and so are the roles. ZEC holders can pay in from a shielded wallet and shield again afterwards, but the lock itself is transparent. Code starts after Orchard activation.

## What changes from DIN↔BTC

Zcash transparent is a Bitcoin-script chain, so most of DIN↔BTC carries over. The work is in the Zcash transaction format, its signature hash, and expiry heights. Baseline: `docs/design/din-btc-atomic-swaps.md` on dinero-main.

| Part | DIN↔BTC | DIN↔ZEC v1 |
| --- | --- | --- |
| Dinero lock (Taproot HTLC) | as designed | unchanged, byte for byte |
| Roles | Alice sells DIN, holds the secret, locks first | unchanged |
| Counter-chain lock | P2WSH HTLC | P2SH HTLC on a transparent `t3` address (no SegWit on Zcash) |
| Hash and timelock opcodes | OP\_SHA256, OP\_CHECKLOCKTIMEVERIFY | the same opcodes |
| Transaction format | Bitcoin, BIP 143 sighash | Zcash v5, ZIP 244 sighash (BLAKE2b, branch id) |
| Stuck transactions | fee ladder, CPFP child | no RBF, no CPFP: pay a fixed multiple of the ZIP 317 fee; a claim the wallet can re-sign may also expire (ZIP 203) and go again |
| Fees | fee-rate market | ZIP 317 conventional fee, set by action count |
| Amounts | satoshis, uint64 | zatoshis, uint64 (21M × 10^8 fits) |
| Counterparty keys | 33-byte compressed pubkey | the same |
| Watching | bitcoind RPC | Zebra RPC, or lightwalletd for phones |
| Funding | the user's bitcoind wallet | any Zcash wallet, including a shielded one, pays a per-swap deposit address; the swap engine then funds the lock itself |
| Privacy | public on both chains | public on both chains; both legs share hash h, so they are linkable |

## Protocol

Alice sells DIN and holds the secret s, with h = SHA256(s). Bob pays ZEC. Defaults follow DIN↔BTC: T\_zec = offer time + 48 h, T\_din = offer time + 96 h.

1. **Offer.** Alice posts a `dinswap2o` offer with chain = zec: both amounts, h, her DIN refund key, her ZEC claim pubkey, T\_zec, T\_din, N\_din, N\_zec and an expiry. Her wallet first passes the DIN balance check.
2. **Deposit, then accept.** Bob's wallet shows a per-swap deposit address (`t1`, a key the swap engine owns). He pays the ZEC amount plus two conventional fees (0.0002 ZEC) from any wallet, shielded included. Once the deposit has confirmed and covers the total, his wallet sends `dinswap2a` with his DIN claim key, his ZEC refund pubkey and his ZEC refund address. Until Alice locks, he can withdraw the deposit at any time.
3. **DIN lock.** Alice funds the Dinero Taproot HTLC, unchanged from DIN↔BTC.
4. **ZEC lock.** After N\_din confirmations Bob's engine checks the DIN lock and confirms no Zcash network upgrade activates before T\_zec plus the refund margin. It builds the funding transaction from the deposit to the P2SH HTLC, pre-signs the refund rungs, gets the tower's ack, and only then broadcasts.
5. **Claim ZEC.** After N\_zec confirmations Alice checks the P2SH script and that the value is at least the amount. Before her cut-off she claims the ZEC to her own address, which reveals s in the claim's scriptSig.
6. **Claim DIN.** Bob's wallet or tower reads s from the Zcash mempool or a block and claims the DIN.
7. **Timeouts.** If Alice never claims, Bob refunds the ZEC once T\_zec passes. If Bob never locks, Alice refunds the DIN once Dinero MTP reaches T\_din.

## Zcash HTLC

The ZEC lock is the DIN↔BTC HTLC script, wrapped in P2SH instead of P2WSH. Zcash script has both opcodes it needs: zcashd's interpreter implements OP\_SHA256 and OP\_CHECKLOCKTIMEVERIFY, and CLTV is in both the block and the standard verify flags.

```text
OP_IF
    OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <alice_zec_pubkey> OP_CHECKSIG
OP_ELSE
    <T_zec_unix> OP_CHECKLOCKTIMEVERIFY OP_DROP <bob_zec_pubkey> OP_CHECKSIG
OP_ENDIF
```

| Path | scriptSig | nLockTime | Sequence | nExpiryHeight |
| --- | --- | --- | --- | --- |
| Claim (Alice) | \<sig> \<s> OP\_TRUE \<redeem script> | 0 | final | tip + 40 (re-signed if it expires) |
| Refund, wallet (Bob) | \<sig> OP\_FALSE \<redeem script> | T\_zec | non-final | tip + 40 |
| Refund, tower rung | as above, signed at lock time | T\_zec | non-final | 0 = never expires (ZIP 203) |

Transactions are v5 with a transparent bundle only and SIGHASH\_ALL under ZIP 244. That digest commits to the consensus branch id, every spent amount and script, nLockTime and nExpiryHeight. Two consequences follow:

- A pre-signed rung must carry nExpiryHeight 0, or it dies about 40 blocks after signing.
- A rung signed before a Zcash network upgrade is invalid after it. The tower cannot re-sign, so swaps whose window crosses an announced activation height are refused (see Deadlines).

**Lock-time rule.** Consensus compares a time lock with the block's own timestamp, not with the median of past blocks (zcashd `ContextualCheckBlock` sets no MTP flag; Zebra checks against block time). Relay uses MTP. A block's timestamp may run up to MTP + 90 min, so a miner can confirm Bob's refund up to about 90 minutes before MTP reaches T\_zec. The engine therefore treats a refund as possible from T\_zec − 90 min, and Alice's cut-off leaves room for that.

A height-based CLTV would avoid this, but Zcash block times drift with hashrate, so a height deadline would wander against the wall-clock T\_din. Time locks keep one clock across both chains, as in DIN↔BTC.

## Deadlines, confirmations and fees

The deadlines are the DIN↔BTC ones; what is new is the 90-minute early-refund allowance, the upgrade check, and ZIP 317 fees. Zcash targets one block per 75 s.

| Parameter | Value | Why |
| --- | --- | --- |
| T\_zec | offer time + 48 h (default) | same as T\_btc |
| T\_din | offer time + 96 h; at least T\_zec + 24 h | Bob needs a day to claim DIN after s appears late |
| Alice's claim cut-off | T\_zec − 6 h | covers 90-min early refunds, a claim expiring and going again, and node lag |
| Bob's go/no-go before locking ZEC | abort if T\_din is under 36 h away, or Zcash MTP lags the wall clock by over 2 h | as DIN↔BTC |
| Upgrade check | refuse if a Zcash activation height falls before T\_zec + 24 h, projected at 75 s per block plus 10% | a ZIP 244 signature binds the branch id, so tower rungs would die |
| N\_zec (Alice waits) | 40 blocks, about 50 min, scaled up with amount like N\_din | protects Alice from a reorg of Bob's lock before she reveals s |
| Deposit confirmations | 10 blocks | only shows the deposit exists; Bob's own coins until the lock |
| Funding fee | 2 × conventional = 20,000 zat (0.0002 ZEC), part of the deposit | deposit input + HTLC output = 2 logical actions |
| Claim and wallet-refund fee | 2 × conventional, taken from the HTLC output | the input is about 300 bytes = 2 actions |
| Tower refund rung | one rung at 4 × conventional (40,000 zat) | 4× is ZIP 317's weight cap, so it ranks first; without RBF, extra rungs would only conflict |

ZIP 317's conventional fee is 5,000 zat × max(2, logical actions). A transparent input counts as one action per 150 bytes, an output one per 34 bytes. Blocks admit at most 50 unpaid actions, so paying at least the conventional fee is what keeps a transaction minable.

## Funding and claiming ZEC

Bob pays a per-swap deposit address from any wallet, and the swap engine builds the lock from it. Dinero has no Zcash wallet to fund from, because Zebra has none. Funding straight from an outside wallet would break two DIN↔BTC rules: the tower must ack before the lock is broadcast, which needs the funding outpoint in advance, and the balance check needs to see the coins.

**Deposit.** One fresh `t1` address per swap, from key m/SWAP'/net'/6'/i'. The HTLC keys use m/SWAP'/net'/5'/i', a separate branch from the BTC keys at 2'.

- Any wallet can pay it. Zashi and other shielded wallets send to a `t1` address, so the source of the ZEC stays shielded.
- **Short payment:** accept stays blocked, and the wallet shows the shortfall in ZEC.
- **Overpayment or several payments:** all deposit outputs are swept into the funding transaction, and the excess goes to Bob's refund address as change. The fee is recomputed from the real input count.
- **Withdraw:** until the funding transaction is broadcast, `swap.cancel` sweeps the deposit to any address Bob names, at the conventional fee. That covers Alice never locking, and offer expiry.

**Claim.** Alice's claim pays a transparent address she names, which may be the transparent receiver of her Zashi wallet, and her wallet then shields it. A claim straight into an Orchard note would need Zcash Orchard proving inside Dinero; it is left for later (Owner decisions).

**Privacy.** Both legs are transparent and share h, so anyone can link the DIN lock to the ZEC lock. The deposit links to the ZEC lock too. What stays private is where Bob's ZEC came from, if he paid from shielded, and where Alice's goes once she shields it.

## Balance checks

A wallet can only offer or accept what it holds. The DIN side uses the rule from the DIN↔ETH design; on the ZEC side the deposit itself is the check.

| When | Who | Check | Refused with |
| --- | --- | --- | --- |
| Posting an offer | DIN seller | Spendable DIN ≥ amount + maximum funding fee + DIN reserved by other open offers and running swaps | offer exceeds available balance (X DIN spendable, Y already committed to open offers) |
| Accepting | ZEC buyer | Confirmed deposit ≥ ZEC amount + funding fee | deposit is short by X ZEC |
| Funding the DIN lock | DIN seller | Re-checked at signing | swap aborts; Bob has locked nothing |
| Funding the ZEC lock | ZEC buyer | Deposit still unspent and confirmed | swap aborts; Alice refunds DIN at T\_din |

Alice needs no ZEC: her claim fee comes out of the HTLC output. Bob's refund fee comes out of it too. Each check ships with a regression test that fails without it.

## Watching Zcash

Desktops and the tower watch through a Zebra node; phones use lightwalletd raw-transaction calls. zcashd is no longer an option: a vendor notice (Tatum) says it halted on July 18, 2026 at height 3,417,100, and Zebra has no wallet, so the engine signs everything itself.

| Need | Zebra JSON-RPC | lightwalletd (phones) |
| --- | --- | --- |
| Lock depth and the chain tip | getblockchaininfo, getblock, getbestblockhash | GetLatestBlock, GetTransaction |
| Upgrade activation heights | getblockchaininfo | from a desktop or tower node in v1 |
| Deposit coins | getaddressutxos | GetAddressUtxos |
| s in a claim, mempool | getrawmempool + getrawtransaction | GetMempoolStream (raw transactions; closes at each new block) |
| s in a claim, mined | getblock with full transactions | GetTaddressTransactions (raw) |
| Broadcast | sendrawtransaction | SendTransaction |

**Raw transactions only.** s sits in the claim's scriptSig. lightwalletd's CompactTxIn carries only the outpoint it spends, so compact block and mempool streams can never show s.

**Who trusts what.** Bob accepts s from any source once SHA256(s) = h; the only risk is a server hiding it. So Bob's tower runs its own Zebra, and a phone checks two independent lightwalletd servers. Alice must see Bob's lock truthfully before she reveals s: her own Zebra, or two servers that agree on the lock and its depth.

**Watchtower.** The tower watches Zcash for s and claims DIN with Bob's pre-signed DIN rungs, as today. It also holds the single ZEC refund rung and broadcasts it once MTP passes T\_zec. It holds no Zcash key.

Open point: whether Zebra's getaddresstxids lists the transaction spending a P2SH output. Until a regtest run confirms it, the watcher scans the mempool and every new block for spends of the HTLC outpoint.

## Changes to the swap engine and offer format

ZEC is a second racing chain behind the `CounterChain` plug-in that the DIN↔ETH work introduces. The state machine and the Dinero leg do not change.

| Area | Change |
| --- | --- |
| Plug-in | ZEC implements `CounterChain` with the racing flag on: after T\_zec, claim and refund can both land, as on Bitcoin |
| Transactions and signing | v5 transparent building, ZIP 244 sighash and t-address encoding come from the reviewed librustzcash crates (zcash\_primitives, zcash\_transparent, zcash\_address) through `rust/zcash_swap`, which becomes a dependency of `rust/orchard_backend` at v8.2.1 so the release ships one Rust static library. No hand-written BLAKE2b or v5 serialization in C++; the tree's only BLAKE2b is inside the vendored Orchard crate |
| Lock-time model | refund treated as possible from T\_zec − 90 min; broadcast once MTP ≥ T\_zec |
| Funding | new states for the deposit: AwaitingDeposit, DepositConfirmed; FundBtcHtlc becomes FundCounterHtlc, which sweeps the deposit |
| Upgrade guard | at offer, accept and lock: read activation heights, refuse a window that crosses one |
| Expiry | claims and wallet refunds carry tip + 40 and are re-signed if they expire; tower rungs carry 0 |
| Offer format | `dinswap2o` / `dinswap2a` from DIN↔ETH with chain = zec: amounts in zatoshi (uint64), 33-byte ZEC pubkeys, N\_zec, T\_zec |
| Record | `dinswap2r` stores the deposit outpoints, the funding transaction and the tower rung |
| Keys | m/SWAP'/net'/5'/i' HTLC key, m/SWAP'/net'/6'/i' deposit key |
| RPC | `swap.*` gains `chain=zec`; `swap.status` shows the deposit address, amount due and confirmations; `swap.cancel` sweeps an unused deposit |
| Config | `swap.zec_rpc_*` for Zebra; `swap.zec_lightwalletd` (two URLs) for phones |

Test networks: Zcash testnet and Zebra regtest. Mainnet addresses start `t1`/`t3`, testnet and regtest `tm`/`t2`.

## Failure handling

Every stop ends with each side holding its own coins, or both swapped; the rows say how.

| What goes wrong | What happens |
| --- | --- |
| Bob never deposits | No accept is sent; the offer expires |
| Bob deposits; Alice never locks DIN | `swap.cancel` sweeps the deposit back; Bob pays one conventional fee |
| Bob accepts, then never locks ZEC | Alice refunds DIN once Dinero MTP reaches T\_din, as in DIN↔BTC; her DIN sits locked until then |
| Deposit spent elsewhere before the lock | The lock cannot be built; the swap aborts and Alice refunds DIN at T\_din |
| A Zcash upgrade is announced mid-swap and lands inside the window | Bob's wallet refunds by re-signing under the new branch id; the tower rung is void, so the wallet alerts that Bob must be online after T\_zec |
| Reorg removes Bob's lock before N\_zec | Alice keeps waiting; she reveals nothing until the lock is N\_zec deep |
| Alice's claim is stuck at a low fee | It expires after 40 blocks and her wallet re-signs it at a higher fee before the cut-off; s is already public, so Bob claims DIN either way |
| Alice misses her cut-off | She does not claim; she refunds DIN at T\_din and Bob refunds ZEC after T\_zec |
| A miner sets block times ahead | The refund confirms up to about 90 min before MTP reaches T\_zec; the 6 h cut-off already covers it |
| A lightwalletd server hides s | The second server or Bob's tower (own Zebra) sees it |
| Bob offline at T\_zec | The tower broadcasts the refund rung |
| Bob offline when s appears | The tower claims DIN with the pre-signed DIN rungs, as today |

## Qualification and milestones

Nothing ships to mainnet until each test below has been seen to run in the CI log and to fail with its fix removed.

- ZIP 244 sighash and v5 serialization match the official zcash-test-vectors.
- The HTLC claim and refund are accepted by a Zebra regtest node; a refund before T\_zec is rejected; a claim with a wrong preimage is rejected.
- A tower rung with nExpiryHeight 0 is mined long after signing; one with the default expiry is not.
- An offer whose window crosses a regtest activation height is refused.
- The deposit: short, exact, overpaid and split payments; cancel sweeps it back.
- Two-chain end-to-end on dinerod regtest plus Zebra regtest: happy path, Alice silent, Bob silent, Bob offline with the tower.
- A miner timestamp at MTP + 90 min confirms the refund early, and Alice's cut-off still holds.
- The phone path reads s from GetMempoolStream and from a mined block.

Milestones after activation:

1. The balance check and the tower fix on DIN↔BTC (shared with DIN↔ETH milestone 1).
2. The `CounterChain` plug-in split (shared with DIN↔ETH milestone 2).
3. Zcash transaction FFI over librustzcash, with the test vectors.
4. Zebra watcher, deposit flow and upgrade guard.
5. Runner, tower and `swap.*` RPC; two-chain regtest suite.
6. dinero-qt Swap tab: ZEC in the chain picker, the deposit QR code and status.
7. Zcash testnet beta, then mainnet with the BTC beta caps.
8. Phone support over lightwalletd.

Steps 3–5 need no external audit, so they can run while the DIN↔ETH contract is out for audit.

## Owner decisions

Decided by the owner on 2026-10-28: all five as recommended below (#2, the per-swap deposit, confirmed first).

| # | Question | Recommendation |
| --- | --- | --- |
| 1 | Build order: ZEC before, after, or alongside ETH? | Alongside: ZEC steps 3–5 while the ETH contract is in audit |
| 2 | Funding: per-swap deposit (one extra fee, tower-safe) or paying the HTLC straight from an outside wallet? | Per-swap deposit |
| 3 | Claim into shielded ZEC directly in v1? | No: transparent claim, then shield in the user's wallet; revisit after v1 |
| 4 | N\_zec default | 40 blocks (about 50 min), scaled up with amount |
| 5 | Caps and offers | BTC beta caps; DIN sellers post offers, as in DIN↔ETH |

## Sources

- [ZIP 203: Transaction Expiry](https://zips.z.cash/zip-0203)
- [ZIP 317: Proportional Transfer Fee Mechanism](https://zips.z.cash/zip-0317)
- [ZIP 244: Transaction Identifier Non-Malleability](https://zips.z.cash/zip-0244)
- zcashd source: [main.cpp](https://github.com/zcash/zcash/blob/master/src/main.cpp) (lock-time cutoff, expiry), [interpreter.cpp](https://github.com/zcash/zcash/blob/master/src/script/interpreter.cpp) (CLTV, SHA256), [chain.h](https://github.com/zcash/zcash/blob/master/src/chain.h) (MTP + 90 min), [policy.h](https://github.com/zcash/zcash/blob/master/src/policy/policy.h) (standard flags), [consensus/params.h](https://github.com/zcash/zcash/blob/master/src/consensus/params.h) (75 s spacing)
- [Zebra lock\_time\_has\_passed](https://zebra.zfnd.org/internal/zebra_consensus/transaction/check/fn.lock_time_has_passed.html)
- [zebra-rpc RpcServer methods](https://docs.rs/zebra-rpc/latest/zebra_rpc/methods/trait.RpcServer.html)
- lightwallet-protocol: [service.proto](https://github.com/zcash/lightwallet-protocol/blob/main/walletrpc/service.proto), [compact\_formats.proto](https://github.com/zcash/lightwallet-protocol/blob/main/walletrpc/compact_formats.proto)
- [Tatum: zcashd end of life](https://docs.tatum.io/changelog/zcash-zcashd-end-of-life-migration-to-zebrad) (vendor notice, not an official Zcash page)
- Dinero: `docs/design/din-btc-atomic-swaps.md`, `include/wallet/swap/swap_manager.h` (48 h / 96 h defaults), and the DIN ↔ ETH Atomic Swaps design
