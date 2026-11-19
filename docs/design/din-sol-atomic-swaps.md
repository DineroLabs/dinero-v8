# DIN ↔ SOL Atomic Swaps

Status: **design, owner decisions recorded 2026-10-28.** No consensus change. Nothing here is built yet; code starts after Orchard activation (H150,000). Target release: v8.2.1.

DIN↔SOL uses the unchanged Dinero Taproot HTLC on one side and a small, immutable Solana HTLC program on the other. Roles stay as in DIN↔BTC: the DIN seller holds the secret and locks first. Code starts after Orchard activation.

## What changes from DIN↔BTC and DIN↔ETH

Solana is closest to DIN↔ETH: a contract holds the coins, claim and refund run in exclusive windows, and anyone may submit either. What is new is Solana's account model, its clock, and a short transaction lifetime.

| Part | DIN↔ETH | DIN↔SOL |
| --- | --- | --- |
| Dinero lock and roles | unchanged | unchanged |
| Counter-chain lock | immutable Solidity contract | immutable Solana program (upgrade authority removed) |
| Where the coins sit | contract balance, per-id record | one escrow account per swap, a PDA derived from the swap id |
| Assets | ETH, USDC | SOL, USDC (classic SPL Token only) |
| Amounts | uint256 | u64 lamports or token base units; no 256-bit change needed |
| Keys | secp256k1, 20-byte address | ed25519, 32-byte address (base58) |
| Hash of s | sha256 precompile | sha256 syscall |
| Clock | block.timestamp | Clock sysvar unix\_timestamp, a stake-weighted median of validator votes |
| Finality | finalized checkpoint, about 13 min | finalized commitment, seconds |
| Pre-signed transactions | not needed | not possible past about 1 minute (blockhash expiry), and not needed: claim and refund are permissionless |
| Liveness risk | low | past multi-hour network halts; wider claim margin |

## Protocol

Alice sells DIN and holds s, with h = SHA256(s). Bob pays SOL or USDC. Both fund from per-swap deposit addresses the swap engine owns, as in DIN↔ZEC, so the engine signs every Solana transaction itself. Defaults: T\_sol = offer time + 48 h, T\_din = offer time + 96 h.

1. **Offer.** Alice posts `dinswap2o` with chain = sol: amounts, the asset (SOL or USDC), h, her DIN refund key, her Solana recipient address, T\_sol, T\_din, N\_din, and an expiry. Her wallet passes the DIN balance check, and her per-swap Solana key holds at least 0.01 SOL for the claim fee.
2. **Deposit, then accept.** Bob pays the amount plus 0.01 SOL for rent and fees to his per-swap deposit address, from any wallet. Once it is finalized, his wallet sends `dinswap2a` with his DIN claim key, his Solana refund address and a lock nonce. Until he locks, `swap.cancel` returns the deposit.
3. **DIN lock.** Alice funds the Dinero Taproot HTLC, unchanged.
4. **SOL lock.** After N\_din confirmations, Bob's engine checks the DIN lock, gets the tower's ack, and calls `lock` on the program from the deposit key.
5. **Claim SOL.** Alice computes the swap id herself and reads the escrow account at finalized commitment. She checks the owner, terms and balance, simulates `claim`, then sends it before her cut-off. The claim pays her fixed recipient address and puts s in the transaction's instruction data.
6. **Claim DIN.** Bob's wallet or tower reads s from that transaction and claims the DIN.
7. **Timeouts.** If Alice never claims, anyone may call `refund` once the Solana clock reaches T\_sol; it pays Bob's fixed refund address. Alice refunds DIN once Dinero MTP reaches T\_din.

**Once s is sent, keep claiming.** The cut-off governs only Alice's first send. A Solana node that saw the transaction may have s even if it never landed, so after the first send her wallet re-signs and resends until the claim lands or T\_sol arrives.

## Solana HTLC program

One small program with three instructions, deployed once per cluster. After a verifiable build, its upgrade authority is set to none, so nobody can change it. The program id is hard-coded in the client, never taken from an offer.

```rust
// id = sha256(program_id || sender || recipient || refund_to || mint || amount || hashlock || timelock || nonce)
// escrow PDA = ["htlc", id]; for USDC the vault is the escrow PDA's associated token account
lock(recipient: Pubkey, refund_to: Pubkey, mint: Option<Pubkey>, amount: u64,
     hashlock: [u8; 32], timelock: i64, nonce: [u8; 32])   // signer: sender (pays amount + rent)
claim(preimage: [u8; 32])   // any signer; only while clock.unix_timestamp < timelock
refund()                    // any signer; only once clock.unix_timestamp >= timelock
```

| Rule | Why | Test |
| --- | --- | --- |
| The escrow PDA comes from the id, which hashes every term plus the nonce; `lock` fails if the account already exists | Alice derives the address herself and reads one account; a replayed lock cannot double-fund | lock twice with one nonce: the second fails |
| Claim only before timelock, refund only from it | claim and refund never race | claim at timelock fails; refund at timelock − 1 fails |
| `claim` checks sha256(preimage) = hashlock with the sha256 syscall | same h as the DIN leg | a wrong preimage fails |
| Payouts go to the stored recipient or refund\_to, whoever signs | a front-run claim or refund still pays the right party; the tower needs no key of Bob's | a claim signed by a stranger pays the recipient |
| `claim` and `refund` re-create the payee's token account if it was closed (payer: the submitter) | a recipient closing their USDC account cannot block the claim | close the account, then claim |
| Escrow and vault are closed on payout; rent goes back to the sender | no stranded lamports | sender's balance after a round trip |
| Only classic SPL Token mints; Token-2022 rejected | transfer fees and hooks could change what arrives | a Token-2022 mint fails |
| Timelock between now + 1 h and now + 7 d | as in DIN↔ETH | the bounds |
| SOL amount at least 0.01 SOL | a transfer that would leave a new account below rent exemption fails | 0.001 SOL fails |

USDC is frozen-able: Circle's freeze authority could freeze the vault, and then neither claim nor refund works. That is the same issuer risk as USDC on Ethereum, and it is disclosed in the UI.

## Deadlines, clock and finality

Alice's cut-off sits 24 h before T\_sol, because Solana has halted for up to about 19 hours (February 2023). Finality is fast, so confirmation waits are short.

| Parameter | Value | Why |
| --- | --- | --- |
| T\_sol | offer time + 48 h (default) | same as T\_btc |
| T\_din | offer time + 96 h; at least T\_sol + 24 h | Bob has a day to claim DIN after a last-minute claim |
| Alice's first-send cut-off | T\_sol − 24 h | outlasts the longest halt on record with room to spare |
| Bob's go/no-go before locking | abort if T\_din is under 36 h away, or the Solana clock lags the wall clock by over 2 h | as DIN↔BTC; a lagging clock means a stalled cluster |
| Alice's wait on Bob's lock | finalized commitment | under Tower BFT, 31+ confirmed blocks on top (about 13 s at 400 ms slots); seconds or less under Alpenglow |
| Deposit wait | finalized | the deposit is Bob's own until the lock |

**The Solana clock.** `Clock.unix_timestamp` is the stake-weighted median of validators' vote timestamps. Agave bounds it against the PoH estimate since the epoch began: at most 25% fast, at most 150% slow. Moving it needs most of the stake, and it does not advance during a halt. Deadline decisions use the later of the wall clock and the latest finalized block's timestamp, as in DIN↔ETH.

**Halts.** Seven full halts happened from December 2020 to February 2024 (about 4.5 to 19 h each), and none since, per Helius. A halt freezes both windows, so it moves no coins by itself. The danger is a halt after Alice's first send that lasts past T\_sol, which is why the cut-off is a full day early.

## Fees, rent, keys and pre-signed transactions

Nothing on the Solana side is pre-signed. A transaction names a recent blockhash and is dead once that blockhash is more than 150 slots old (`MAX_PROCESSING_AGE`), about a minute. A tower cannot hold signed transactions for days without durable-nonce accounts. Claim and refund are permissionless, so it does not need to.

| Item | Cost | Paid by |
| --- | --- | --- |
| Transaction fee | 5,000 lamports per signature, plus a priority fee from recent prioritization fees | the submitter |
| Escrow account rent | about 0.002 SOL, returned on payout | Bob, at lock |
| USDC vault account rent | about 0.002 SOL, returned on payout | Bob, at lock |
| Alice's USDC account, if missing | about 0.002 SOL, not returned (it becomes Alice's) | Bob, at lock |
| Alice's claim | under 0.0001 SOL | Alice's per-swap key (0.01 SOL deposit; the rest swept to her recipient) |
| Tower refund | under 0.0001 SOL | the tower's own fee key, funded once with 0.01 SOL |

The rent figures are approximate and are read live from `getMinimumBalanceForRentExemption` before each lock.

**Keys.** ed25519, derived with SLIP-0010, hardened only, at m/SWAP'/net'/7'/i'. One per-swap key is Bob's deposit and lock signer, or Alice's claim-fee payer. OpenSSL 3.5.6 in the tree already signs ed25519 (Tor onion keys use it). Message encoding comes from the reviewed solana-message and solana-transaction crates through the `rust/` FFI, not hand-written C++.

**Watchtower key.** The tower holds no key of Bob's for Solana. Its own fee key pays to submit `refund`, which can only pay Bob's refund address.

## Who must check what, and balance checks

Trust is lopsided, as on Ethereum. Alice risks her SOL claim if an RPC lies about Bob's lock; Bob only needs s, which proves itself.

| Who | Must check | Against |
| --- | --- | --- |
| Alice, before claiming | the escrow account at the id she computed: owned by the program, terms match the offer, balance or vault holds the amount, timelock = T\_sol | her own RPC node, or two independent providers that agree, at finalized |
| Alice, before sending | `simulateTransaction` of the claim succeeds | the same RPC |
| Bob | SHA256(s) = h | any source |

**Simulate before sending.** A failed Solana transaction is still recorded, fee paid and instruction data included. A claim that reverts still leaks s, so the wallet never sends a claim whose simulation failed.

**Front-running is harmless.** Anyone who sees s can submit the claim, but it pays only Alice's fixed recipient. Private submission is optional.

**Balance checks**

| When | Who | Check | Refused with |
| --- | --- | --- | --- |
| Posting an offer | DIN seller | spendable DIN ≥ amount + maximum funding fee + DIN reserved by other open offers and running swaps; claim-fee key holds ≥ 0.01 SOL | offer exceeds available balance / add 0.01 SOL for the claim fee |
| Accepting | SOL or USDC buyer | finalized deposit ≥ amount + 0.01 SOL for rent and fees (USDC: token balance ≥ amount as well) | deposit is short by X |
| Locking | both | re-checked at signing | swap aborts before anything moves |

## Watchtower and watching Solana

Wallets and the tower watch Solana through RPC providers, not their own node. A Solana validator or RPC node needs far more hardware than a home machine, unlike Zebra or bitcoind.

| Need | RPC call |
| --- | --- |
| Bob's lock, for Alice | `getAccountInfo` on the escrow PDA (and vault), commitment finalized |
| s, as soon as it lands | `logsSubscribe` or `accountSubscribe` on the escrow, then `getTransaction` |
| s, polling fallback | `getSignaturesForAddress` on the escrow PDA, then `getTransaction`; read the preimage from the claim's instruction data |
| Deposit | `getBalance` / `getTokenAccountBalance`, finalized |
| Rent and fees | `getMinimumBalanceForRentExemption`, `getRecentPrioritizationFees` |
| Send | `simulateTransaction`, then `sendTransaction`; resend until it lands or the blockhash expires, then re-sign |

**Two providers.** Bob's wallet and tower each query two independent providers, so one that hides the claim does not keep s from Bob. Alice needs two that agree on her lock read, or her own RPC node.

**Tower duties.**

- Watch for s and claim DIN with Bob's pre-signed DIN rungs, as today.
- Once the Solana clock reaches T\_sol and no claim has landed, submit `refund` with its own fee key.

The tower needs one more config entry (the providers) and a fee key; nothing per swap is signed for Solana.

## Changes to the swap engine and offer format

SOL is a third `CounterChain` plug-in, with the exclusive-window flag set as for Ethereum. The state machine and the Dinero leg do not change.

| Area | Change |
| --- | --- |
| Plug-in | Solana implements `CounterChain`; exclusive windows, like Ethereum |
| Program client | build `lock`, `claim` and `refund`; derive the id and escrow PDA locally |
| Transactions | legacy messages via the solana-message and solana-transaction crates over the `rust/` FFI; compute-unit limit and price set per send |
| Signing | ed25519 via the vendored OpenSSL; SLIP-0010 derivation at m/SWAP'/net'/7'/i' |
| Funding | deposit states from DIN↔ZEC (AwaitingDeposit, DepositConfirmed); FundCounterHtlc calls `lock` from the deposit key |
| Sending | resend-until-landed loop with blockhash refresh; simulation gate on every claim |
| Offer format | `dinswap2o` / `dinswap2a` with chain = sol: asset (SOL or the USDC mint), u64 amount, 32-byte recipient and refund addresses, lock nonce in the accept |
| Record | `dinswap2r` stores the swap id, escrow address and claim signatures sent |
| RPC | `swap.*` gains `chain=sol`; `swap.status` shows the deposit address and amount due |
| Config | `swap.sol_rpc` (two URLs), `swap.sol_cluster` |

Program ids and the USDC mint are fixed per cluster in the client: mainnet-beta, devnet and a local test validator.

## Failure handling

Every stop but one ends with each side holding its own coins, or both swapped. The exception is a Solana halt after Alice's first send that lasts more than 24 h.

| What goes wrong | What happens |
| --- | --- |
| Bob never deposits | No accept; the offer expires |
| Bob deposits; Alice never locks DIN | `swap.cancel` returns the deposit, minus one fee |
| Bob accepts, then never locks | Alice refunds DIN once Dinero MTP reaches T\_din |
| An RPC lies about Bob's lock | Two providers disagree, or simulation fails; Alice does not claim and refunds DIN at T\_din |
| Alice's claim does not land | Her wallet re-signs with a fresh blockhash and resends until it lands or T\_sol arrives |
| Solana halts before Alice's first send | She waits; if the cut-off passes, she does not claim and both sides refund |
| Solana halts after her first send and resumes after T\_sol | The claim window has closed. If s leaked, Bob could claim DIN and refund SOL. The 24 h cut-off margin exists to make this need a halt longer than any on record |
| Alice closes her USDC account | `claim` re-creates it |
| Circle freezes the USDC vault | Neither claim nor refund works until it is unfrozen; disclosed issuer risk |
| A provider hides s from Bob | The second provider or the tower sees it |
| Bob offline at T\_sol | The tower submits `refund` |
| Bob offline when s appears | The tower claims DIN, as today |

## Qualification and milestones

The program is the one piece that needs an external audit. Nothing ships to mainnet until each test below has been seen to run in the CI log and to fail with its fix removed.

- Program unit tests, one per rule in the program table, plus fuzzing of instruction data and account substitution: a wrong escrow, vault, mint or token program is refused.
- The deployed program's verifiable build matches the source, and its upgrade authority is none.
- Two-chain end-to-end on dinerod regtest plus solana-test-validator: happy path, Alice silent, Bob silent, Bob offline with the tower; SOL and USDC.
- A claim that would revert is never sent (simulation gate).
- A claim that does not land is re-signed and lands before T\_sol.
- The clock check: the test validator's clock is warped past T\_sol, and claim fails while refund works.
- Deposit: short, exact and over; cancel returns it.

Milestones after activation:

1. The balance check and tower fix on DIN↔BTC (shared).
2. The `CounterChain` plug-in split (shared).
3. The HTLC program and its test suite; send it for audit.
4. Solana client FFI: messages, ed25519, SLIP-0010.
5. Solana watcher, deposit flow and v2 formats with chain = sol.
6. Runner, tower and `swap.*` RPC; two-chain end-to-end suite.
7. dinero-qt Swap tab: SOL and USDC in the picker.
8. Devnet beta; then mainnet with small caps once the audit passes.

## Owner decisions

Decided by the owner on 2026-10-28: all five as recommended below.

| # | Question | Recommendation |
| --- | --- | --- |
| 1 | Build order | Last of the three: ETH and ZEC first; SOL reuses their plug-in, deposit flow and v2 formats |
| 2 | Assets in v1 | SOL and USDC, as with ETH |
| 3 | Program framework | Anchor, pinned to one version: its account checks remove the most common Solana bug class, and auditors know it well |
| 4 | Funding | Per-swap deposit, as decided for ZEC |
| 5 | Caps and offers | BTC beta caps (lower for USDC); DIN sellers post offers |

## Sources

- Agave source: [stake\_weighted\_timestamp.rs](https://github.com/anza-xyz/agave/blob/master/runtime/src/stake_weighted_timestamp.rs) (median, 25% fast and 150% slow bounds)
- solana-sdk source: [clock/src/lib.rs](https://github.com/anza-xyz/solana-sdk/blob/master/clock/src/lib.rs) (MAX\_PROCESSING\_AGE = 150)
- [Anza docs: Commitments](https://docs.anza.xyz/consensus/commitments) (finalized = 31+ confirmed blocks; Alpenglow)
- [Anza docs: Validator timestamp oracle](https://docs.anza.xyz/implemented-proposals/validator-timestamp-oracle)
- [Helius: Solana outages, complete history](https://helius.dev/blog/solana-outages-complete-history)
- Dinero: `docs/design/din-btc-atomic-swaps.md`, `include/wallet/swap/swap_manager.h` (48 h / 96 h defaults), the DIN ↔ ETH and DIN ↔ ZEC designs

Not opened, from general knowledge and to be checked during milestone 3: rent amounts (read live anyway), the USDC mint addresses, and Anchor's current version.
