# DIN ↔ BTC swaps — v1 implementation plan

Companion to `din-btc-atomic-swaps.md` (the design). Status: **proposal for
review, 2026-10-04.** Already built: the pure HTLC script/transaction builders
(`include/wallet/swap/htlc.h`, PR #829). Everything below waits for review.

Scope of v1: **two people, copy-paste offers, small amounts, dinero-qt only,
watchtower required for Bob.** No order book, no maker bot, no mobile, no XMR.

## 1. Modules

| Module | File(s) | Depends on |
|---|---|---|
| HTLC builders (done) | `wallet/swap/htlc.{h,cpp}` | consensus Taproot helpers |
| Offer format | `wallet/swap/offer.{h,cpp}` | htlc.h |
| Fee ladder | `wallet/swap/fee_ladder.{h,cpp}` | htlc.h, signer |
| Swap state machine | `wallet/swap/swap_engine.{h,cpp}` | all above, wallet DB, watchers |
| Dinero watcher | `wallet/swap/din_watcher.{h,cpp}` | local node (chainstate, mempool) |
| Bitcoin watcher | `wallet/swap/btc_watcher.{h,cpp}` | user's Electrum server or bitcoind RPC |
| Watchtower | `tools/dinero-swap-tower` (small daemon) | htlc.h, both watchers |
| RPCs | `rpc/methods_swap.cpp` (`swap.*`) | swap_engine |
| UI | dinero-qt "Swap" tab | RPCs |

Keys: one fresh key pair per swap per chain, derived from the wallet seed at a
dedicated swap path (never reused, recoverable from the seed).

## 2. Offer (copy-paste string)

Versioned, checksummed text blob (bech32m-style, human-pasteable). Fields:

```
version, role (DIN-seller | BTC-seller), din_amount, btc_amount,
payment_hash (Alice's), din_claim_key, din_refund_key,
btc_claim_key, btc_refund_key, T_din_unix, T_btc_unix,
N_din, N_btc, network (mainnet/regtest), expiry
```

Each side **re-derives and checks** every value it relies on (timelock gaps,
amounts, keys belong to the right party, both HTLC scripts) before acting.
An offer never contains a secret or a private key.

## 3. State machine (persisted, resumable, idempotent)

Alice (DIN seller, holds `s`):

```
NEW → OFFER_SENT → ACCEPTED → DIN_LOCKED → (Bob's BTC lock seen, N_btc deep)
    → BTC_CLAIM_BROADCAST (s revealed) → BTC_CLAIM_CONFIRMED → DONE
refunds:  DIN_LOCKED --(T_din_unix, no Bob claim)--> DIN_REFUND_BROADCAST → REFUNDED
cut-off:  if not claimed by T_btc_unix − 6 h → ABANDON_BTC → wait → DIN refund
```

Bob (BTC seller):

```
NEW → OFFER_RECEIVED → ACCEPTED → (Alice's DIN lock seen, N_din deep, checks pass)
    → BTC_LOCKED → TOWER_ARMED → (s seen on BTC) → DIN_CLAIM_BROADCAST
    → DIN_CLAIM_CONFIRMED → DONE
refunds:  BTC_LOCKED --(T_btc_unix, no s)--> BTC_REFUND_BROADCAST → REFUNDED
abort:    before BTC_LOCKED, any §6.1 abort rule → ABORTED (nothing at risk)
```

Rules:
- Every transition is written to the wallet DB **before** the action it
  triggers (crash-safe; on restart, re-derive what is on chain and continue).
- Broadcasts are idempotent (re-sending the same tx is harmless).
- Deadlines are evaluated from each chain's MTP, displayed as wall-clock.
- `s` is stored encrypted with the wallet; it leaves the wallet only inside
  Alice's BTC claim.

## 4. Watchers

- **Dinero**: local node — HTLC output creation (with depth), spends of it from
  mempool and blocks, reorgs that remove either.
- **Bitcoin**: Electrum protocol (scripthash subscribe for the P2WSH output and
  its spends) or bitcoind RPC; no Dinero-run server. The watcher reports depth,
  mempool spends, and the claim witness (→ `ExtractPreimageFromBtcClaim`).
- Both report reorgs; the engine re-evaluates state on every reorg.

## 5. Fee ladder

For each pre-signed claim/refund: 8 rungs, feerate roughly doubling from the
current estimate to a ceiling; all pay the same destination, `SIGHASH_DEFAULT`,
replaceable. Rungs are generated and signed when the swap is armed and handed
to the tower. Ceiling is capped so a rung never pays more than a set fraction
of the swap value (user-visible).

On Dinero the rungs are not escalated (no replace-by-fee on default nodes):
the tower picks the DIN rung by how close `T_din_unix` is. See design §6.2.

## 6. Watchtower

Small separate process Bob runs on an always-on machine (same PC is allowed
but discouraged). Holds only pre-signed transactions + the payment hash; no
private keys. Duties: broadcast Bob's BTC refund rung after `T_btc_unix` if `s`
is unseen; insert `s` into Bob's DIN claim rungs and broadcast when `s` appears;
escalate rungs if not confirming. Authenticated local API (arm / status / disarm).
Availability only — residual risks per design §6.2.

## 7. RPCs (local wallet only)

`swap.offer`, `swap.accept`, `swap.status`, `swap.list`, `swap.claim`,
`swap.refund`, `swap.armtower`, `swap.cancel` (only before funds are locked).
Add `swap.*` to the public RPC guard denylist in the same change.

## 8. dinero-qt Swap tab

Create offer / paste offer → review screen (amounts, rate, both deadlines in
local time, worst-case lock duration, required confirmations, amount cap, "you
must keep the watchtower running until …") → progress timeline per swap →
manual claim/refund buttons as a fallback. Big warnings for the two
liveness duties.

## 9. Tests (gate for each milestone)

- Unit: offer encode/decode/checksum and every validation refusal; state
  machine transitions incl. crash at every state; fee ladder rungs all valid
  and all pay the same destination; reorg handling.
- Two-chain regtest harness (dinerod + bitcoind): happy path, both refunds,
  every race and stall case in design §9, swap across the 60 s activation,
  tower with Bob offline, Utreexo/CSN node validating both spends.
- Negative: wrong `h`, wrong keys, timelock gap too small, insufficient depth,
  oversized amount — all refused before funds move.

## 10. Milestones

1. Offer format + validation (unit tests).
2. Fee ladder (unit tests through consensus verifier, like #829).
3. Engine + Dinero watcher + regtest harness (Dinero side only).
4. Bitcoin watcher + two-chain regtest happy path and refunds.
5. Watchtower + race/stall suite.
6. RPCs + guard denylist; dinero-qt tab.
7. External review; small-amount mainnet beta between known testers.

Each milestone is a separate PR, reviewed before the next starts.
