# DIN ↔ ETH atomic swaps — design

Status: **design, owner decisions recorded 2026-10-28.** No consensus change. Nothing here is built yet; code starts after Orchard activation (H150,000). Target release: v8.2.1.

DIN↔ETH reuses the DIN↔BTC swap unchanged on the Dinero side; on the Ethereum side a small, ownerless contract replaces the Bitcoin script. Design only: code starts after the H150,000 activation (about Nov 10).

## What changes from DIN↔BTC

The Dinero leg, the roles and the swap engine stay; the Ethereum leg, its watcher and the tower's job change. Baseline: `docs/design/din-btc-atomic-swaps.md` and `include/wallet/swap/` on dinero-main.

| Part | DIN↔BTC today | DIN↔ETH |
| --- | --- | --- |
| Dinero lock | Taproot, NUMS key; claim leaf (Bob + 32-byte secret), refund leaf (Alice, CLTV timestamp) | Unchanged, byte for byte |
| Who starts | Alice (sells DIN) makes the secret, locks first, has the later deadline | Same |
| Other-chain lock | P2WSH script | Immutable swap contract (one deployment per chain, same address) |
| Claim vs refund on the other chain | Both valid after `T_btc`; they race | Exclusive: claim only before `T_eth`, refund only from `T_eth` |
| Who can submit claim/refund | Key holder only | Anyone; money always goes to the address fixed at lock |
| Alice waits for | `N_btc` blocks | The `finalized` block tag (about 13 min) |
| Bob's tower on the other chain | Signed BTC refund fee ladder | Nothing to pre-sign; refund is a public call, any time after `T_eth` |
| Assets | BTC | ETH; USDC as an option |
| Amount field | `uint64` sats | 256-bit (wei overflows `uint64` above about 18.4 ETH) |

The exclusive window is the main gain. Alice can no longer claim late, so Bob always gets the full `T_din − T_eth` to claim his DIN. Bob's refund is no longer urgent.

## Protocol

Alice sells DIN and holds the secret; Bob sells ETH (or USDC). Alice reveals the secret on Ethereum and Bob reuses it on Dinero.

1. **Offer.** Alice picks a random 32-byte secret `s`, sets `h = SHA256(s)`, and publishes an offer. It names: amounts, `h`, chain (Ethereum or Base), asset, her ETH payout address, her DIN refund key, `T_eth`, `T_din`, `N_din`.
2. **Accept.** Bob answers with his DIN claim key, his ETH refund address, his per-swap ETH sending address and a lock nonce, bound to the offer's id. Both sides can now compute the lock id themselves.
3. **Alice locks DIN** in the existing Taproot HTLC: Bob claims with `s`; Alice refunds after `T_din`.
4. **Bob checks and locks ETH.** He waits until the DIN lock is `N_din` blocks deep and runs the abort rules. Then he calls `lock()` on the contract with `h`, Alice's payout address, his refund address and `T_eth`.
5. **Alice checks, simulates, claims.** She reads the lock at the id she computed, from contract storage (the swaps(id) getter) at the `finalized` tag, from her own node or a quorum of providers. She simulates `claim(id, s)` with `eth_call`, then submits it. The contract pays her address and records `s`.
6. **Bob claims DIN.** His wallet or tower reads `s` from the `Claimed` event or contract storage, checks `SHA256(s) = h`, and claims the DIN before `T_din`.
7. **If anyone stops:** after `T_eth` anyone can call `refund(id)`, which pays Bob's refund address. After `T_din` Alice refunds her DIN.

The DIN seller is always the maker. An ETH holder who wants DIN cannot post an offer in v1; they can only accept one (owner decision 4).

## Ethereum contract

One small, immutable contract with three calls, deployed once per chain. It has no owner, proxy, pause, fee to Dinero Labs or self-destruct. Nobody can change or stop it after deployment, including us.

```solidity
function lock(address recipient, address refundTo, address token, uint256 amount,
              bytes32 hashlock, uint64 timelock, uint256 tip, bytes32 nonce)
    external payable returns (bytes32 id);
function claim(bytes32 id, bytes32 preimage) external;   // before timelock
function refund(bytes32 id) external;                    // from timelock on
event Locked(bytes32 indexed id, address sender, address recipient, address refundTo,
             address token, uint256 amount, bytes32 hashlock, uint64 timelock, uint256 tip);
event Claimed(bytes32 indexed id, bytes32 preimage);
event Refunded(bytes32 indexed id);
```

| Rule | Why | Test (each must fail with the rule removed) |
| --- | --- | --- |
| `claim` checks `sha256(preimage) == hashlock` with the SHA-256 precompile, not keccak | Same hash as the Dinero leaf's `OP_SHA256`; `bytes32` fixes the length like `OP_SIZE 32` | One secret opens both the Dinero leaf and the contract; a keccak or 31-byte preimage is rejected |
| `claim` only while `block.timestamp < timelock`; `refund` only from `timelock` on | Exactly one exit is open at any time, so the two never race | Boundary test at `timelock − 1` and `timelock` |
| Claim pays `recipient`, refund pays `refundTo`, never `msg.sender` | Front-running a claim or refund can't redirect money, so anyone (tower, relayer) may submit | Third party submits both; balances land only at fixed addresses |
| `id = keccak256(chainid, contract, sender, every term, nonce)`; a used id reverts | One id per lock, never reusable across chains or contracts. Including the sender stops a stranger pre-locking Bob's id. Full terms are stored behind a public swaps(id) getter. | Duplicate lock reverts; same terms on another chain id give a different id |
| State set to Claimed/Refunded before any transfer; one exit per id | No reentrancy, no double payout | Reentrant token and recipient contracts |
| Payout that fails is credited to the payee, who withdraws it later (`withdraw(token, to)`, callable only by the payee) | A recipient contract that rejects ETH, or a frozen USDC address, can't make `claim` revert after `s` is public | Rejecting recipient: claim succeeds, credit recorded, withdraw to a second address works |
| `lock` checks the contract's token balance rose by exactly `amount`; ETH requires `msg.value == amount` | Rejects fee-on-transfer and rebasing tokens | Fee-on-transfer token reverts |
| `timelock` between now + 1 h and now + 7 days; `tip ≤ amount` | Same 7-day lock bound as the BTC swap | Out-of-range values revert |
| Optional `tip` paid to `msg.sender` on claim (0 by default) | Lets a relayer submit Alice's claim when she holds no ETH (decision 3), without a second deployment later | Claim with tip pays recipient `amount − tip` |

The contract is about 120 lines, built with a pinned compiler. It is deployed with CREATE2 to the same address on Ethereum and Base, with source verified on Sourcify. The address is hard-coded in the client per chain id. **An offer can never supply a contract address**, or a malicious offer could point Bob at a fake contract.

## Deadlines and finality

All deadlines are Unix timestamps, as in the BTC swap. On Ethereum, `block.timestamp` is fixed by the 12-second slot schedule, so the contract's clock tracks wall time closely. Base gets longer margins to cover its sequencer.

| Parameter | Ethereum | Base | Why |
| --- | --- | --- | --- |
| `T_eth` (ETH refund opens, claim closes) | lock + 24 h | lock + 36 h | Bounds how long Bob's ETH is stuck if Alice disappears |
| `T_din` (Alice's DIN refund opens) | ≥ `T_eth` + 24 h | ≥ `T_eth` + 24 h | Bob's guaranteed window after the latest possible reveal |
| Alice waits for Bob's lock | `finalized` tag (about 13 min) | `finalized` tag (about 20 min, once its Ethereum batch is 2 epochs old) | A reorged-out lock can't cost her the secret |
| Alice's claim cut-off | `T_eth` − 6 h | `T_eth` − 18 h | Base: room for forced inclusion through Ethereum (up to the 12-hour sequencing window) |
| `N_din` (Bob waits) | Scaled by amount, min 30 blocks | Same | Unchanged; DIN is the small-hashrate chain |
| Bob acts on `s` | At once, from any block or mempool | Same | A secret proves itself by its hash |
| Bob's abort rules | As BTC: `T_din` ≥ 36 h away, Dinero MTP lag ≤ 2 h, DIN lock `N_din` deep and unspent, offer not expired | Same, plus Base `finalized` head not over 1 h behind | Same reasons |

Open point: 12 h is the OP Stack default sequencing window. Confirm Base's configured value from its on-chain SystemConfig before the Base parameters are frozen.

## Who must check what

Alice's risk is a false lock, and Bob's is a missed secret. Most users will read Ethereum through hosted providers (Infura, Alchemy, public RPCs), so each side gets a different rule.

|  | What a lying or broken provider can do | Rule |
| --- | --- | --- |
| **Alice** | Report a lock that doesn't exist, isn't final, or has the wrong terms. Her claim then reverts but still shows `s` in calldata, and Bob takes her DIN. | Before claiming: read the lock from contract storage at `finalized`, from her own node or 2 of 2 independent providers that agree byte for byte. Check every term against the offer. Simulate the claim with `eth_call`. |
| **Bob** | Hide the claim, so Bob misses `s` and Alice refunds the DIN after `T_din`. | Poll several providers. Accept `s` from any of them once `SHA256(s) = h`; no agreement needed. Alert if they disagree for more than 10 min. |

**Private submission for Alice.** On Ethereum she sends the claim through a private relay (Flashbots Protect or MEV Blocker), so `s` is public only once the claim is included. If it never lands, `s` may stay secret and she can refund her DIN as normal. The client still treats `s` as public once sent, because a relay can leak it.

A light client such as Helios could replace the provider quorum later. That is out of scope for v1.

## Balance checks

A wallet can only offer or accept what it can actually pay. Today's DIN↔BTC `swap.offer` checks caps, amounts, deadlines and fee terms, but not the balance; the shortfall surfaces only when the DIN lock is funded after acceptance. No funds are lost (the DIN seller locks first), but a buyer can accept an offer that can never fill, and one wallet can post several offers against the same coins.

| When | Who | Check | Refused with |
| --- | --- | --- | --- |
| Posting an offer | DIN seller | Spendable DIN ≥ amount + maximum funding fee + DIN committed to other open offers and running swaps | offer exceeds available balance (X DIN spendable, Y already committed to open offers) |
| Posting an offer | DIN seller | ETH on the claim chain ≥ estimated claim gas, unless a relayer tip is set | not enough ETH on \<chain> to pay claim gas; add ETH or set a relayer tip |
| Accepting an offer | ETH or USDC buyer | Asset balance ≥ amount (+ committed to other running swaps), and ETH ≥ gas for lock plus refund | accept exceeds available balance |
| Funding the lock | Both | Re-check at signing; the balance can change between offer and lock | the swap aborts before anything is sent; the counterparty has not committed yet |

Committed amounts come from the swap store: every open offer and every swap not yet settled or refunded reserves its amount plus maximum fee. Cancelling an offer releases its reservation. An expired offer stays reserved until cancelled, because the DIN seller's wallet can still start a swap from it. The offer-time check is advisory to the counterparty (they cannot see your wallet); the funding-time check is the one that guarantees nothing is overspent. Each check ships with a regression test that fails without it, including two offers that together exceed the balance.

## Base and USDC

Both make swaps cheaper or more useful, and each adds a party users must trust that Ethereum L1 with ETH doesn't have.

| Option | Gain | Added trust point | Handling |
| --- | --- | --- | --- |
| **Base** | Gas in cents, not dollars | One sequencer can delay or censor a claim for up to the sequencing window. Base's upgrade keys can change the chain's contracts. | Longer `T_eth` and Alice cut-off (deadlines table). Forced inclusion through Ethereum's OptimismPortal if a claim isn't included within 1 h. Upgrade-key risk named in the app. |
| **USDC** | DIN for a dollar asset; likely the most-wanted pair | Circle can freeze any address, including the swap contract, and can upgrade the USDC contract. | Client allowlist per chain (ETH, USDC; USDT only after a separate check). Credit-on-failure covers a frozen payee. If the contract itself is frozen, locked USDC is stuck. Stated in the app; lower caps for USDC. |

USDC funding uses `permit` (EIP-2612) plus `lock` in one transaction where the token supports it; otherwise approve, then lock. The contract stays token-neutral. The allowlist lives only in the client, so a token can be dropped without redeploying.

## Keys, gas and the watchtower

The user's own Ethereum wallet only receives or pays in. Signing for the swap happens with a per-swap key the Dinero wallet derives from its seed.

- **Key path:** `m/SWAP'/net'/4'/i'` (secp256k1). `1'` is the DIN key, `2'` BTC, `3'` the DIN CPFP sweep. The index is never reused, as today.
- **Bob funds through it.** The app shows the per-swap address. Bob sends `amount` plus a gas allowance from any wallet (MetaMask, exchange). The app saves the lock nonce in the encrypted session before signing `lock()`, so a resubmission after a crash hits "used id reverts" instead of funding twice. It then sweeps leftovers to Bob's refund address once settled. Refunds go to `refundTo` (his own wallet), not to the per-swap key.
- **Alice needs gas to claim.** Her payout address is any address she types. The claim is sent from her per-swap key, which needs about 60,000 gas of ETH (cents on Base, more on Ethereum). If she holds no ETH, the optional `tip` lets a relayer submit it for her (decision 3).
- **Transactions:** EIP-1559 with an EIP-155 chain id, `maxFeePerGas` set at 3× the current base fee, raised by replacement if not included within 3 blocks. Ethereum replace-by-fee is normal, unlike Dinero.
- **Missing pieces in the tree:** no keccak-256, RLP or Ethereum transaction code exists today (grep of `src/` and `include/`). Use one small, reviewed library for keccak and RLP rather than writing them; signing reuses the vendored libsecp256k1.

**Watchtower.** Bob's tower keeps its DIN job exactly as today: signed DIN claim rungs without the secret, plus the CPFP child. It adds an Ethereum watcher that inserts `s` when it appears. It needs no Ethereum key of Bob's: refund is a public call, and Bob's refund is no longer time-critical. The tower may call `refund` from its own small gas key as a convenience. The mainnet rule "Bob needs an armed tower before funding" stays.

## Changes to the swap engine and offer format

The engine is BTC-shaped in a few places that would break silently for Ethereum. Step one is splitting out a per-chain plug-in, with the existing BTC swap tests passing unchanged.

| Where | Today | Change |
| --- | --- | --- |
| `SwapOffer` (`offer.h`) | `btc_amount_sat` `uint64`, 33-byte BTC pubkeys | Offer: chain id, asset (ETH or token address), 256-bit amount, 20-byte payout address, `tip`. Accept: Bob's 20-byte refund address, per-swap sending address and lock nonce. |
| Wire format | `dinswap1o` / `dinswap1a`, version 1 | `dinswap2o` / `dinswap2a`, version 2. Old decoders reject it by prefix; new decoders still read v1 BTC offers. |
| `HtlcObservation::output_value` (`engine.h`) | `uint64` | 256-bit, so wei above about 18.4 ETH can't overflow |
| `Observations::btc_mtp_unix` | Bitcoin MTP | Two separate inputs. Deadline decisions use the later of wall clock and the latest head's timestamp (Bitcoin MTP for BTC). Ethereum's `finalized` tag only answers whether Bob's lock is final: it lags about 13 min and can stall for hours. |
| Claim/refund logic in `Step()` | Assumes BTC claim and refund race after `T_btc` | Per-chain flag: racing (BTC) or exclusive (ETH). In exclusive mode Bob's refund is never urgent and Alice's claim is impossible after `T_eth`. |
| Watchers / IO (`runner.h` `SwapChainIo`) | `ObserveBtc`, `FundBtc`, `BroadcastBtc` | A `CounterChain` interface: observe, prepare funding, claim, refund, time. One implementation for BTC, one for EVM (Ethereum and Base). |
| Record (`dinswap1r`) | v1 | v2 adds chain id and asset; v1 records still load |
| Tower package | BTC refund rungs | Optional per chain; EVM packages carry the watch spec only |

The Dinero-side code (HTLC builder, DIN watcher, fee rungs, CPFP, encrypted store) does not change.

## Failure handling

Every stop leaves each side able to recover its own coins. The one exception is Alice's claim failing after the secret is out, which the cut-off and simulation guard against.

| Situation | Result |
| --- | --- |
| Bob never locks ETH | Alice refunds DIN after `T_din` |
| Alice never claims | Anyone calls `refund` after `T_eth`; Alice refunds DIN after `T_din` |
| Alice's claim lands just before `T_eth` | Bob still has 24 h until `T_din` to claim DIN (exclusive window) |
| Alice's claim reaches a mempool but isn't included by `T_eth` | It reverts; `s` may be public; Bob refunds ETH and can take the DIN. Prevented by the cut-off, private submission and fee replacement. |
| Provider shows Alice a fake or unfinal lock | Stopped by the storage read at `finalized` from her node or two agreeing providers, plus `eth_call` |
| Provider hides Alice's claim from Bob | Other providers or the tower still see it; alert on disagreement |
| Payee rejects ETH or is frozen by Circle | Claim succeeds; amount credited for `withdraw` |
| Swap contract frozen by Circle (USDC) | USDC stuck in the contract; ETH swaps unaffected. Named risk, lower caps. |
| Base sequencer censors Alice's claim | Forced inclusion through Ethereum; Base cut-off leaves 18 h for it |
| Ethereum reorg before `finalized` | Alice doesn't claim until the lock is final; Bob's use of `s` is safe in any case |
| DIN reorg below `N_din` | Bob doesn't lock until `N_din`; amount caps bound the attack |
| Bob's wallet offline after the reveal | Tower inserts `s` into his signed DIN claim |
| Offer points at another contract or token | Refused: contract address and token allowlist come from the client |

## Qualification before release

Nothing ships until each item below has run (confirmed in its log) and fails with its rule removed.

- **Contract (Foundry):** a unit test per rule in the contract table, each neutered; fuzz tests over amounts, times and preimages; invariants: contract balance equals open locks plus credits per token, no id both claimed and refunded.
- **One secret, both chains:** the same `s` opens the Dinero claim leaf (`ScriptVerifier`) and the contract; a 31- or 33-byte value and a keccak preimage fail on both.
- **Two-chain end to end:** regtest dinerod plus anvil, using anvil's time control. Covers the happy path, both refunds, claim at `T_eth − 1` and refusal at `T_eth`, restart at every state, tower-only DIN claim, and an anvil reorg before finality.
- **Lying providers:** a fake provider shows Alice a lock that isn't there (she must not claim); a provider hides the claim from Bob (the second provider or tower catches it).
- **Mainnet fork:** anvil forked from Ethereum with real USDC: permit + lock, claim, a Circle-frozen recipient (credit path), a frozen contract.
- **Base:** the same suite on Base Sepolia, plus a forced-inclusion claim through the OptimismPortal.
- **BTC regression:** after the plug-in split, all existing `Swap*` ctests pass unchanged.
- **Gates:** an external contract audit, then a Sepolia beta between known testers with small caps, before any mainnet deployment.

## Milestones after activation

Coding starts after H150,000 is stable. Each milestone is its own PR, reviewed before the next one starts, as with DIN↔BTC.

1. Add the offer-time balance and reservation check to DIN↔BTC (see Balance checks), and push the pending watchtower fix (`claude/swap-tower-hardening`).
2. Split the engine into the per-chain plug-in; BTC tests unchanged.
3. Contract plus Foundry suite; send it for external audit (the audit runs in parallel with 4–7).
4. EVM library: keccak, RLP, EIP-1559 signing, ABI encoding for the three calls.
5. EVM watcher with the provider rules, and the v2 offer and record formats.
6. Runner, tower and `swap.*` RPC integration; two-chain anvil end-to-end suite.
7. dinero-qt Swap tab: chain and asset picker, gas and trust notes.
8. Sepolia and Base Sepolia beta; then mainnet with small caps after the audit passes.

## Owner decisions

Decided by the owner on 2026-10-28: all five as recommended below. For decision 4, only DIN sellers can post offers in v1; ETH holders can still accept them. This keeps one role layout shared with DIN↔BTC. The person who holds the secret always locks DIN first, so the counterparty never commits before the small-hashrate chain is deep.

| # | Decision | Options | Recommendation |
| --- | --- | --- | --- |
| 1 | Networks | Ethereum, Base, or both | Both, same contract address; Base with the longer deadlines |
| 2 | Assets | ETH only, or ETH + USDC | ETH + USDC; USDC with lower caps and the freeze risk stated |
| 3 | Alice's gas | She must hold a little ETH, or the relayer `tip` | Keep `tip` in the contract (0 by default); ship v1 with "hold some ETH" and add relayers later |
| 4 | Who can make offers | DIN sellers only, or also ETH sellers | DIN sellers only in v1; a reversed-role design later if demand shows |
| 5 | Amount caps for beta and mainnet | Per asset | Same caps as the BTC beta; lower for USDC |

## Sources

- DIN↔BTC design and code on dinero-main: `docs/design/din-btc-atomic-swaps.md`, `include/wallet/swap/engine.h`, `offer.h`, `swap_manager.h` (key paths)
- [OP Stack forced transactions (Optimism docs)](https://docs.optimism.io/stack/transactions/forced-transaction): 12-hour sequencing window, 30-minute max deposit drift (OP Stack defaults)
- [Base transaction finality (Base docs)](https://docs.base.org/base-chain/network-information/transaction-finality): L1 batch finality about 20 min
- [Circle required block confirmations](https://developers.circle.com/stablecoins/docs/required-block-confirmations) (not opened; search summary only): Base about 13 min
