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

Safety: Alice can only learn the BTC by revealing `s`; once `s` is public Bob
has until `T_din` to take the DIN. Bob never reveals anything.

## 4. Dinero-side output (exact)

Taproot output, **internal key = BIP341 NUMS point**
`50929b74c1a04954b78b4b6035e97a5e078a5a0f28ec96d547bfee9ace803ac0`
(no key path: the coins move only through a leaf; same constant as the CTV
covenant profile and, after #827, the escrow builder). Two leaves:

```
claim  : OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <bob_xonly> OP_CHECKSIG
refund : <T_din> OP_CHECKLOCKTIMEVERIFY OP_DROP <alice_xonly> OP_CHECKSIG
```

- `OP_SIZE 32` pins the preimage size on both chains (prevents a preimage that
  is valid on one chain and rejected on the other).
- Keys are fresh per swap (derived from the wallet seed at a dedicated swap
  path), never reused addresses.
- Claim spend witness: `<sig_bob> <s> <claim_script> <control_block>`.
- Refund spend: `nLockTime = T_din`, input `nSequence < 0xffffffff`, witness
  `<sig_alice> <refund_script> <control_block>`.

## 5. Bitcoin-side output

Standard P2WSH HTLC (widest wallet/explorer support), same `h`:

```
OP_IF
   OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <h> OP_EQUALVERIFY <alice_pubkey>
OP_ELSE
   <T_btc> OP_CHECKLOCKTIMEVERIFY OP_DROP <bob_pubkey>
OP_ENDIF
OP_CHECKSIG
```

P2TR with the same two leaves is an optional v2 (cheaper, more private).
Bob funds the BTC HTLC **from any Bitcoin wallet** — it is just an address.
The Dinero app holds only the per-swap BTC claim/refund keys and sweeps to a
BTC address the user types in.

## 6. Timeouts and confirmations

Use **block heights on each chain**, converted from wall-clock targets with
margins (Dinero 120 s blocks today, 60 s after v8.1.13 activation; BTC ~600 s,
high variance).

| Parameter | Proposed default | Why |
|---|---|---|
| `T_btc` (Bob refund) | now + 24 h of BTC blocks (≈144) | Bob's coins are never locked longer than a day |
| `T_din` (Alice refund) | now + 48 h of DIN blocks | ≥ 24 h after `T_btc`, so Bob always has a full day to claim DIN after `s` is revealed |
| `N_din` (Bob waits) | scaled by amount (see below), min 30 blocks | DIN is a small-hashrate chain; deep confirmation before Bob commits BTC |
| `N_btc` (Alice waits) | 2–3 BTC blocks | standard |
| Claim deadline in app | warn at `T_din − 12 h`, auto-claim | Bob's software must claim long before the refund opens |

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
5. **UI**: dinero-qt "Swap" tab first; mobile later via the embedded node.
6. **Negotiation v1**: copy-paste offer strings (amounts, rate, `h`, both
   pubkeys, `T_din`, `T_btc`). v2: an order board (Nostr relay or a signed
   offer board) and an optional maker bot to seed liquidity.

## 8. Failure handling

| Situation | Result |
|---|---|
| Bob never locks BTC | Alice refunds DIN after `T_din` |
| Alice never claims BTC | Bob refunds BTC after `T_btc`; Alice refunds DIN after `T_din` |
| Alice claims BTC near `T_btc` | still safe: Bob has until `T_din` (≥ 24 h later) to claim DIN |
| Bob's software offline after `s` revealed | must come back before `T_din`; app warns; watchtower option in v2 |
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
- Time-based vs height-based lock behaviour on Dinero (`nLockTime` MTP rules)
  if time locks are ever used; v1 uses heights only.
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
| Regtest two-chain harness + qualification | medium |
| Order board / maker bot | medium, optional for v1 |
