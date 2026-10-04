# DIN ↔ BTC atomic swaps (Taproot HTLC) — design

Status: **proposal, 2026-10-04.** No consensus change. Nothing here is built yet.

## 1. Goal

Let two people trade DIN for BTC (and later LTC or any chain with SHA-256
hashlocks and absolute timelocks) **without a trusted third party**: either both
legs complete, or each side gets its own coins back after a timeout.

Non-goals for v1: DIN ↔ XMR (needs adaptor signatures, see §10), a public order
book, and post-quantum (P2MR) outputs (P2MR is single-key with no script, so it
cannot hold a contract).

## 2. What the chain already supports (mainnet, verified on `dinero-main`)

| Need | Dinero | Evidence |
|---|---|---|
| Output type for a contract | Taproot (P2TR) only — consensus spends P2PKH, P2WPKH, P2TR, P2MR; no P2SH/P2WSH | `src/consensus/script_validation.cpp` (spend dispatch) |
| Hashlock | `OP_SIZE`, `OP_SHA256`, `OP_EQUALVERIFY` in tapscript | `src/consensus/tapscript_interpreter.cpp` |
| Absolute timelock | `OP_CHECKLOCKTIMEVERIFY`, BIP65 semantics (height or time, same type as `nLockTime`) | `tapscript_interpreter.cpp`; tx-level locks enforced from height 111,000 (`contextual_locks.h`) |
| Signatures | BIP340 Schnorr, `OP_CHECKSIG` in tapscript | `script_validation.cpp` |
| Multi-party signing | PSBT with tapscript leaf signing (`wallet.createfundedpsbt`, `processpsbt`, `signpsbt`, `finalizepsbt`) | `psbt_signer.cpp` |
| Script-tree helpers | leaf hash, branch hash, output-key tweak, control blocks | `src/wallet/taproot_template_builder.cpp` |

**Refund lock = CLTV (absolute), not CSV.** Stateless Utreexo nodes may not be
able to check a relative lock for an input whose creation height they lack
(`block_validation.cpp`, relative-lock path). An absolute lock does not depend
on the input's height. (Inference from the code; confirm in qualification §9.)

## 3. Protocol (classic two-chain HTLC)

Roles: **Alice** has DIN and wants BTC. **Bob** has BTC and wants DIN.
The party who generates the secret must lock first and with the **longer**
timeout. We make the **DIN seller (Alice) the initiator**.

```
1. Alice picks secret s (32 random bytes), h = SHA256(s).
2. Alice locks DIN in a Taproot HTLC:
      claim leaf : Bob can spend with s          (OP_SIZE 32 / OP_SHA256 h / Bob sig)
      refund leaf: Alice can spend after T_din   (CLTV T_din / Alice sig)
3. Bob waits for N_din confirmations of Alice's lock, checks amount/script/T_din.
4. Bob locks BTC in an HTLC with the SAME h:
      claim : Alice can spend with s
      refund: Bob can spend after T_btc          (T_btc well before T_din)
5. Alice waits for N_btc confirmations, claims the BTC → s becomes public on BTC.
6. Bob reads s from Alice's BTC claim, claims the DIN before T_din.
   If Alice never claims: Bob refunds BTC after T_btc; Alice refunds DIN after T_din.
```

Safety: Alice can only take the BTC by revealing `s`; Bob never reveals
anything. **The timelocks only open refunds — they never close the claim
paths.** After `T_btc` both Alice's BTC claim and Bob's BTC refund are valid
and race; after `T_din` both Bob's DIN claim and Alice's DIN refund race. So
safety is conditional on each side acting in time (§6.1), not guaranteed by the
scripts alone.

`T_din` and `T_btc` are **Unix timestamps** checked against each chain's
median time past, never block heights (§6).

## 4. Dinero-side output (exact)

Taproot output, **internal key = BIP341 NUMS point**
`50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0`
(no key path: the coins move only through a leaf; same constant as the CTV
covenant profile and, after #827, the escrow builder). Two leaves:

```
claim  : OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <bob_xonly> OP_CHECKSIG
refund : <T_din_unix> OP_CHECKLOCKTIMEVERIFY OP_DROP <alice_xonly> OP_CHECKSIG
```

- `OP_SIZE 32` pins the preimage size on both chains (prevents a preimage that
  is valid on one chain and rejected on the other).
- Keys are fresh per swap (derived from the wallet seed at a dedicated swap
  path), never reused addresses.
- Claim spend witness: `<sig_bob> <s> <claim_script> <control_block>`.
- `T_din_unix` is a **Unix timestamp** (≥ 500,000,000), not a height — see §6.
- Refund spend: `nLockTime = T_din_unix`, input `nSequence < 0xffffffff`, witness
  `<sig_alice> <refund_script> <control_block>`.

## 5. Bitcoin-side output

Standard P2WSH HTLC (widest wallet/explorer support), same `h`:

```
OP_IF
   OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <alice_pubkey>
OP_ELSE
   <T_btc_unix> OP_CHECKLOCKTIMEVERIFY OP_DROP <bob_pubkey>
OP_ENDIF
OP_CHECKSIG
```

P2TR with the same two leaves is an optional v2 (cheaper, more private).
Bob funds the BTC HTLC **from any Bitcoin wallet** — it is just an address.
The Dinero app holds only the per-swap BTC claim/refund keys and sweeps to a
BTC address the user types in.

## 6. Timeouts and confirmations

**Both refund locks are timestamps, never block heights.** A height lock is a
block count, and Dinero's block rate is not fixed: the v8.1.13 activation
halves the target (120 s → 60 s), ASERT and the difficulty-encoding quirk
produce bursts, and Bitcoin blocks vary widely. A "48-hour" DIN height lock
computed at 120 s becomes ≈24 h if the switch happens mid-swap, wiping out
Bob's claim margin (review finding, 2026-10-04). Timestamp locks keep the
margin in wall-clock time regardless of block rate.

Dinero enforces timestamp `nLockTime` (≥ 500,000,000) against the **parent
block's median time past**, BIP113-style (`include/consensus/contextual_locks.h`,
absolute-lock branch), and that check does not depend on input heights, so
Utreexo/CSN nodes enforce it too. Bitcoin uses the same rule.

| Parameter | Proposed default | Why |
|---|---|---|
| `T_btc_unix` (Bob refund opens) | lock time + 24 h | Bob's BTC is locked at most ~a day if Alice disappears |
| `T_din_unix` (Alice refund opens) | ≥ `T_btc_unix` + 24 h | the gap Bob needs after a late reveal (§6.1) |
| `N_din` (Bob waits) | scaled by amount (see below), min 30 blocks | DIN is a small-hashrate chain; deep confirmation before Bob commits BTC |
| `N_btc` (Alice waits) | 2–3 BTC blocks | standard |
| Alice's claim cut-off | *app policy only*: the app will not claim BTC after `T_btc_unix` − 2 h | lowers the chance of a late reveal; **not a guarantee** — any other client can still claim (§6.1) |

### 6.1 What is actually guaranteed, and the liveness each side owes

Nothing in the scripts expires the claim paths, and median-time lag is not a
bounded quantity (a chain can stall or slow for hours). The real guarantees:

**Bob is safe if, and only if, both hold:**

1. **Prompt BTC refund.** As soon as Bitcoin's MTP reaches `T_btc_unix` and `s`
   has not appeared, Bob broadcasts his BTC refund (pre-signed, high fee,
   fee-bumpable). Until it confirms, Alice can still claim the BTC — that is
   the race. If Alice wins it, `s` appears around `T_btc_unix` plus the race
   time, and Bob still has roughly `T_din_unix − T_btc_unix` minus Bitcoin's
   confirmation delay to act on the DIN side.
2. **Prompt DIN claim.** The moment `s` appears (in Bitcoin's mempool or a
   block), Bob broadcasts his DIN claim and gets it confirmed before Dinero's
   MTP reaches `T_din_unix`.

If Bob is offline through `T_btc_unix`, Alice can claim the BTC **arbitrarily
late** (as long as Bob has not refunded), leaving Bob any window down to zero —
the 24 h gap protects only a Bob who is online (or delegated, §6.2).

**Alice is safe** with no time-critical action: she only reveals `s` by
claiming BTC she then holds. Her only liveness duty is to reclaim her DIN
after `T_din_unix` if Bob never locked (or never claimed) — and after
`T_din_unix` her refund races Bob's claim, so a Bob who learned `s` must have
claimed before then.

**Chain-stall effects (not covered by fixed margins):**

| Stall / slowdown | Effect | Handling |
|---|---|---|
| Bitcoin stalls or slows near `T_btc_unix` | Bitcoin's MTP stays below `T_btc_unix`, so Bob cannot refund while Alice can still claim — the reveal can slip later in wall-clock time, eating Bob's DIN window | Bob claims DIN the instant `s` appears; the app tracks *wall-clock* time left until Dinero's `T_din_unix`, not the Bitcoin deadline |
| Dinero stalls or slows near `T_din_unix` | Dinero's MTP stays low, so Alice's refund is delayed too (in Bob's favour) — but Bob's claim also cannot confirm without blocks | claim broadcast early and fee-bumped; a stall delays both sides equally |
| Fee spike on either chain | claim or refund may not confirm in time | replaceable fees on all claim/refund transactions; CPFP via the sweep output on Bitcoin |

**Abort rules before Bob locks BTC:** Bob's client refuses to lock if
`T_din_unix − now < 36 h`, if either chain's MTP lags wall-clock time by more
than 2 h (signs of a stall), or if Alice's DIN lock has fewer than `N_din`
confirmations.

### 6.2 Watchtower (required for Bob in v1)

Because Bob's safety depends on being online at two moments, v1 ships a
**watchtower**: a process (on Bob's own always-on machine, or a third party he
chooses) that holds

- Bob's **pre-signed BTC refund** (`nLockTime = T_btc_unix`), broadcast as soon
  as Bitcoin's MTP allows if `s` has not appeared; and
- Bob's **pre-signed DIN claim**, with the preimage slot empty. A BIP341
  tapscript signature commits to the transaction and leaf, not to the other
  witness items, so the tower can insert `s` when it appears without holding
  Bob's key. *(Inference from BIP341; must be confirmed against Dinero's
  tapscript sighash in qualification §9.)*

The tower can only complete transactions that pay Bob; it cannot redirect funds.

MTP lags real time (≈6 blocks: ~6–12 min on Dinero, ~1 h on Bitcoin) and can
lag more during stalls; the app always computes deadlines from each chain's
current MTP and shows wall-clock time remaining.

**Reorg / hashrate risk (main DIN-specific risk).** If an attacker can rewrite
`N_din` DIN blocks, Alice could double-spend her lock after Bob locks BTC. Rule:
cap swap size so the cost of out-mining `N_din` blocks exceeds the swap value,
and raise `N_din` with amount. The app shows the cap; makers set their own.

## 7. Software components

1. **Script/tx library** (`src/wallet/swap/`): build both HTLC scripts and the
   DIN Taproot tree (reuse `TaprootTemplateBuilder` leaf/branch/tweak helpers),
   BTC P2WSH address, claim/refund transaction builders, preimage extraction
   from a BTC claim witness. Pure functions + test vectors.
2. **Swap state machine** persisted in the wallet DB: `offered → din_locked →
   btc_locked → btc_claimed(s known) → din_claimed` with refund branches;
   resumable after restart, idempotent.
3. **Chain watchers**: DIN via the local node; BTC via a user-configured
   Electrum server or bitcoind RPC (no Dinero-run custody, no Dinero server in
   the trust path).
4. **Wallet RPCs** (local wallet only): `swap.offer`, `swap.accept`,
   `swap.status`, `swap.claim`, `swap.refund`, `swap.list`. They must never be
   reachable on the public bridge endpoints: add `swap.*` to the public RPC
   guard's denylist when they land (and leave them out of the planned allowlist).
5. **Watchtower** (§6.2): holds Bob's pre-signed BTC refund and DIN claim;
   runs alongside dinero-qt or as a small daemon; required for Bob in v1.
6. **UI**: dinero-qt "Swap" tab first; mobile later via the embedded node.
7. **Negotiation v1**: copy-paste offer strings (amounts, rate, `h`, both
   pubkeys, `T_din`, `T_btc`). v2: an order board (Nostr relay or a signed
   offer board) and an optional maker bot to seed liquidity.

## 8. Failure handling

| Situation | Result |
|---|---|
| Bob never locks BTC | Alice refunds DIN after `T_din` |
| Alice never claims BTC | Bob refunds BTC after `T_btc`; Alice refunds DIN after `T_din` |
| Alice claims BTC after `T_btc_unix` (any client) | races Bob's BTC refund; if Alice wins, Bob must claim DIN before `T_din_unix` — safe only if Bob (or his watchtower) is online (§6.1) |
| Bob offline through `T_btc_unix` | Alice may claim BTC arbitrarily late, shrinking Bob's DIN window toward zero — the watchtower (§6.2) is required in v1 |
| DIN block rate changes mid-swap (60 s activation, bursts) | no effect: both locks are timestamps |
| Bob's software offline after `s` revealed | watchtower inserts `s` into Bob's pre-signed DIN claim and broadcasts it (§6.2) |
| Fee spike on refund/claim | claim/refund txs built with replaceable fees; BTC side CPFP via the sweep output |
| DIN reorg below `N_din` | Bob does not lock until `N_din`; amount caps bound the attack value |
| Wrong `h`, amount, or timeout in counterparty lock | each side verifies the other's script byte-for-byte before acting |

## 9. Qualification (before any release)

- Regtest Dinero + regtest bitcoind end-to-end: happy path, both refund paths,
  claim at the last safe block, restart mid-swap at every state.
- Negative: preimage of wrong size, wrong hash, refund before `T_din` (must be
  rejected by consensus), claim by the wrong key, key-path spend attempt on the
  NUMS output (impossible by construction).
- **Mempool relay** of tapscript CLTV/hashlock spends on Dinero (no special
  tapscript policy found; verify on regtest).
- **Utreexo/CSN node** accepts and validates both spends (confirms the
  CLTV-over-CSV choice).
- Timestamp locks: refund rejected while parent MTP < `T_din_unix`, accepted
  after; include a swap that **crosses the 60 s activation** on regtest and one
  with a block burst, and check the claim margin in wall-clock time.
- CSN/Utreexo node enforces the timestamp lock (parent MTP available).
- **Race tests:** Alice claims BTC just after `T_btc_unix` while Bob's refund
  is in flight (both orderings); claim and refund broadcast in the same block
  window on each chain; Bob offline through `T_btc_unix` with and without the
  watchtower; Bitcoin stall pushing the reveal late (measure Bob's remaining
  wall-clock window); Dinero stall near `T_din_unix`; fee-spike/RBF on every
  claim and refund.
- **Watchtower:** pre-signed DIN claim with `s` inserted later verifies under
  Dinero's tapscript sighash (confirms §6.2's BIP341 inference); tower cannot
  alter outputs.
- Client abort rules (§6.1) refuse to lock under each listed condition.
- External review of the script templates and state machine.

## 10. Later: DIN ↔ XMR

Monero has no scripts, so the swap needs **adaptor signatures** on the DIN side
(COMIT/Farcaster-style). Dinero verifies plain BIP340 Schnorr, so this needs
**no fork** — only new off-chain crypto (Schnorr adaptor sigs; libsecp256k1's
bundled adaptor module here is ECDSA-only and switched off) plus the Monero
key-splitting protocol. Same lock/cancel/refund tree on the DIN side, built with
the §4 tooling. Start only after v1 is proven.

## 11. Effort (rough)

| Piece | Size |
|---|---|
| Script/tx library + vectors | small |
| State machine + persistence + watchers | medium |
| RPCs + dinero-qt Swap tab | medium |
| Watchtower (pre-signed refund/claim, preimage insertion) | small–medium |
| Regtest two-chain harness + qualification | medium |
| Order board / maker bot | medium, optional for v1 |
