# Orchard on Dinero: what to add next, and in what order

Status: proposal for the owner. Nothing here is started except item 1a.
Written 2026-11-25, after v8.2.6 candidate work merged (#976).

## Where Dinero stands

The cryptography is Zcash's own: the `orchard` crate 0.16.0 (Halo 2 proofs, no
trusted setup, `orchard_v2()` bundles, the `FixedPostNu6_2` circuit key), plus
`zcash_note_encryption`, `incrementalmerkletree` and `zip32`. No proof
arithmetic is written locally.

What works today: accounts, receive addresses, shield, private send, unshield,
received history, recovery of an interrupted operation, a phone path.

What Zcash built around the same cryptography over three and a half years,
and Dinero has not: a wallet that syncs fast under load, a light-wallet
protocol, unified addresses, viewing-key export, a fee rule that prices spam,
and independent review. Every Orchard bug fixed for v8.2.6 was in that
surrounding layer (delivery to the wallet, reads blocking delivery, block
readers that cannot read an Orchard block, mining templates, screens), none in
the cryptography.

The five items below are ordered by cost and by how much risk each removes.

---

## 1. The patched `orchard` crate

### Facts (checked 2026-11-25)
- `rust/orchard_backend/vendor/orchard` is the crates.io 0.16.0 archive
  (SHA-256 `4ae7ceb7…706e8f`) with two files changed, 82 lines: a pinned
  `rand 0.8.8` in `Cargo.toml`, and one extra builder entry point,
  `build_with_profile1_shuffle`, in `src/builder.rs`. The other 109 upstream
  files are byte-identical. Verification, the circuit and the wire format are
  untouched.
- Why it exists: a queued operation's construction plan is saved so the exact
  same transaction can be rebuilt after a restart. Plans saved in the first
  format (magic `DNOPLN01`, "profile 1") were built with the older library's
  ordering of actions; the newer library orders them differently. The patch
  keeps the old ordering available for those saved plans.
- New plans are always recorded in the second format (`DNOPLN02`), which uses
  the stock builder (`recovery.rs`, `BuildRng::record`). The patched entry
  point is reached only from `wallet_prover.rs::finish_plan` when a restored
  plan is profile 1.

### 1a. Make it provable (done, branch `claude/orchard-vendor-provenance`)
A CI check downloads the upstream archive, requires its recorded SHA-256, and
fails unless the vendored tree is that archive plus exactly the committed
patch file and the notes file. Needs a pull request to start running.

### 1b. Remove it
Profile-1 plans are a finite set: only operations queued before the format
changed and never finished. Steps:
1. Count them. A wallet read that reports, per account, how many saved
   operations carry a `DNOPLN01` plan (needs the wallet unlocked; the plan is
   stored encrypted). Log the count at unlock. Zero is expected on almost
   every wallet.
2. One release in which a profile-1 operation can still be finished or
   cancelled, and the screen says so for any wallet that has one.
3. The following release refuses to restore a profile-1 plan with a clear
   message (the funds are not at risk: an unfinished operation's reservation
   is released by cancelling it), deletes `build_with_profile1_shuffle` and
   the `[patch.crates-io]` entry, and builds against the unmodified crate.
   The provenance check then asserts "no difference from upstream".

Risk: a user with a years-old queued operation loses the ability to finish
that exact transaction and must re-create it. Mitigated by step 2 and by the
count telling us whether anyone is affected.

Size: small. Touches `wallet_prover.rs`, `recovery.rs`, one RPC field, one
line of Qt. No consensus change.

### 1c. Independent review
Out of scope for this roadmap (owner, 2026-11-25). Recorded only so the gap
is not forgotten: it is the one part of the "patched, not stock" concern that
1a and 1b cannot close.

---

## 2. Viewing-key export and watch-only accounts

### What it is
A full viewing key lets its holder see everything an account receives and
sends, and spend nothing. Uses: bookkeeping, audits, a watch-only wallet on a
second computer, proving a payment.

### Facts
- The backend already derives and can export the 96-byte full viewing key
  (`orchard_wallet.h`, `ExportFullViewingKey`). No RPC exposes it.
- Today an account's stored state is authenticated with the wallet seed, and
  delivery needs the wallet unlocked. A watch-only account has no seed, so it
  needs its own ownership rule. That is the real work, not the export.

### Build
1. Export: `wallet.orchard.exportviewingkey {account}`. Wallet must be
   unlocked; the reply and the screen state plainly that the key reveals the
   account's whole history, past and future.
2. A text encoding for the key with a network prefix and checksum, so a
   mainnet key cannot be pasted into testnet and a typo is caught.
3. Import as a watch-only account: its own catalog entry type, state
   authenticated by a key derived from the viewing key, no reservations, no
   operations queue. Balance and received history only. Every spend path
   refuses it by type, with a test for each.
4. Qt: an "Export viewing key" action behind a confirmation, and a clear
   "watch-only" badge wherever such an account appears.

Decision for the owner: export the full viewing key (sees incoming and
outgoing), or also offer the incoming-only key (sees receipts, not spends).

Size: export is small; watch-only import is medium. Wallet only, no
consensus change.

---

## 3. Fees: first stop larger payments being refused, then price spam

### Why this item changed (stage 0 findings, 2026-11-26)
The first version of this note treated the fee as a spam question. Tracing
and measuring it showed a nearer problem: with the fee every client sends
today, an Orchard transaction with three or more actions is below the
network's existing minimum and is refused.

### Facts
- Admission: the mempool refuses an Orchard transaction whose fee divided by
  its virtual size is below 1 una per virtual byte (`src/daemon/mempool.cpp`,
  "Orchard fee rate below minimum", `DEFAULT_MIN_FEE_RATE`). Reported in
  review, not re-traced here: nothing overrides that default.
- Miner: no minimum of its own. Block selection orders by fee rate and drops
  nothing for a low fee; it relies on admission. No consensus fee minimum was
  found.
- Clients: all three send a fixed fee of 10,000 una (0.0001 DIN). On the
  desktop Orchard page it is an editable default (`qt/src/orchardwidget.cpp`,
  the `orchardFee` field). On both phone apps it is a constant the user
  cannot change. None computes the fee from the transaction's size.
- Sizes, measured with the stock 0.16 builder and verified proofs: an Orchard
  bundle is 2,842 + 3,156 × actions bytes, so 9,154 bytes for 2 actions,
  12,310 for 3 and 28,090 for 8. A 2-action shield seen on mainnet has a
  virtual size a little over 9,300, which matches.
- Therefore: at 10,000 una a 2-action transaction clears the minimum with
  about 690 virtual bytes to spare, and **any transaction with 3 or more
  actions does not** (a private send that has to combine three or more
  notes, for example). Not measured: a 2-action shield with several, or
  post-quantum, transparent inputs, which may use up that margin.
- What happens to the refused operation: the transaction is finalised and
  stored as "signed" before it is submitted, and a retry resubmits the same
  bytes (`src/rpc/orchard_account_rpc.cpp`, the comment "retry retains the
  same Ready transaction, never a fresh proof"), so it is refused every time.
  No `wallet.orchard.*` request cancels an operation. Not yet established:
  whether the notes it reserved stay reserved for good. If they do, this
  locks part of a balance, not just one payment.
- Spam at today's minimum: a block of about a million virtual bytes holds
  about 107 two-action shields, about 0.011 DIN per block.

### Build, in this order
- Stage 0 (done): the facts above.
- Stage 1, the fix: every client computes the fee from the size of the
  transaction it is about to build, `fee ≥ minimum rate × virtual size` with a
  margin, and shows it before the user confirms. The size is known before
  proving (it depends only on the number of actions and the transparent
  parts). The node should offer the number, so three clients do not each
  carry the formula: a field in the queue reply, or a small estimate request.
  The desktop field stays editable but starts from the computed value and
  refuses to go below it.
- Stage 1b, the stuck operation. A signed operation that can never be
  admitted, with no request that cancels it, means notes reserved with no way
  out: this is a correctness item. Establish what such an operation holds,
  then give it a way out (cancel it and release its notes, or let it be
  re-made with a sufficient fee). Alongside that, the node refuses at queue
  time, before anything is reserved or proved, an operation whose fee cannot
  meet the minimum at its size, and says what fee is required. The required
  fee is computed from the real transaction: the Orchard part is exact from
  the action count; the transparent part comes from the actual inputs and
  outputs, whose sizes differ a great deal (post-quantum witnesses), never
  from a constant; plus a margin.
  Reach: phones embed their own node, so this refusal reaches a phone only
  with a new app release, like stage 1. On the desktop it takes effect with
  the node.
- Stage 2: wait until the clients that compute the fee are what people run.
- Stage 3, only then: consider raising the minimum to price spam, per
  virtual byte or per action. Raising it earlier would refuse even 2-action
  payments from every client in the field.

Tests to write first: a three-note private send at the default fee on a
regtest node (expected today: refused, operation left signed), and the same
after stage 1 (accepted, fee shown equals the fee paid).

Decisions for the owner: whether stage 1 and the queue-time refusal go into
v8.2.6 before it is released; the margin; later, the stage 3 rate.

Size: stage 1 is small on the node and small in each client, but it is three
clients and two app-store releases. Stage 1b depends on what the trace finds.

---

## 4. Unified addresses

### What it is
One address string that carries several receivers (for example an Orchard
receiver and a transparent one). The sender's wallet uses the best one it
supports. It removes "which address do I give them?".

### Facts
Dinero's Orchard address is a single-receiver format: Bech32m with prefix
`dinorch` / `tdinorch` / `rdinorch`, a one-byte profile and the 43-byte raw
Orchard address (`rust/orchard_backend/src/wallet.rs`).

### Build
A new address type alongside the existing one, never replacing it: its own
prefix, an ordered list of typed receivers, the standard rule that a wallet
must pick the most private receiver it understands. Encode/decode in the
backend, acceptance in the send forms of both apps, the explorer's address
page.

Decision for the owner: is bundling wanted at all? It is only worth doing if
transparent and Orchard receiving are meant to be offered together. If the
plan is "Orchard addresses for private, transparent addresses for public",
the current format is enough and this item can be dropped.

Size: medium, no consensus change, but it touches every place an address is
typed, shown, scanned or validated, on desktop, phone and explorer.

---

## 5. Fast sync, and whether to have a light-wallet protocol

These are separate questions.

### 5a. Fast sync (the main wallet project after v8.2.6)
How the desktop wallet learns about its money today: the node keeps a full
replay of every block event since Orchard activation; each account is
advanced through it one event at a time; each step captures and re-checks the
whole account catalog. v8.2.6 made that tolerable (reads no longer block
delivery; idle passes are skipped; each shield is checked once; a restart is
minutes shorter). It is still a design whose cost grows with chain length
and with the number of retained revisions, and one that a flood of Orchard
outputs would slow for everyone.

What the Zcash wallets do instead, each of which can be adopted separately:
1. Compact block data: per block, only what a wallet needs to notice its own
   notes (the nullifier, the note commitment, the ephemeral key and the first
   bytes of the ciphertext for each action). Trial decryption runs over this,
   in batches, not over full transactions.
2. Spend before sync: scan the newest blocks first and keep enough of the
   commitment tree to spend a recent note at once, then fill in history in
   the background.
3. A sharded commitment tree, so witnesses are updated without rebuilding
   the tree and a reorganisation undoes only the last shard.
4. A nullifier map, so a spend is recognised without having scanned the
   block that created the note.

Proposed order: 1 (it also caps the cost of a spam flood, together with item
3 above), then 3, then 2 and 4 together.

This is large: a new store, a new scan loop, migration of existing accounts,
and the reorganisation rules for each. It should start with a written design
and a measured baseline (blocks per second scanned, memory, time to first
spendable balance on a wallet of the pilot's size), and be built behind the
existing delivery path until it has matched it on a real wallet.

### 5b. A light-wallet protocol
A server hands a phone the compact blocks from 5a(1), so the phone runs no
node. Dinero's phones embed a node today. The trade:
- For: much less storage, battery and data on the phone; faster first sync.
- Against: the server learns which block ranges a phone asks for and when,
  and the phone must trust the server not to withhold blocks (it can still
  verify what it is given against headers).

Decision for the owner: keep phones as embedded nodes (today), offer a light
mode as an option, or move to light by default. 5a(1) is needed either way;
5b is only the serving side and a product decision.

---

## Suggested sequence

| When | Item |
|---|---|
| Right after v8.2.6 ships | 1a into CI; 1b step 1 (count profile-1 plans); 2 export only |
| As soon as decided | 3 stage 1 and the queue-time refusal (candidate for v8.2.6 itself) |
| Next minor release | 1b step 2; 2 watch-only import; 3 stage 1b |
| v8.3 line | 5a design and baseline, then compact scan data; 1b step 3 (stock crate) |
| Not scheduled | 4 (dropped for now); 5b (waits on 5a); 1c (out of scope) |

## Decisions

Decided by the owner (2026-11-25): outside review is out of scope here.

Proposed in review, not yet confirmed by the owner:
1. Viewing keys: the full viewing key only for now; an incoming-only key
   later if someone asks for it.
2. Fees (revised after stage 0): do not raise the minimum first. Clients
   compute the fee from size, then adoption, then consider the minimum.
3. Unified addresses: dropped for now.
4. Phones: stay embedded nodes; a light mode waits on fast sync.

## What this note does not claim

- That a three-action payment is refused today follows from the measured
  sizes and the admission rule; nobody has yet run one on a node and watched
  it be refused. Whether its reserved notes stay locked is not established.
- No sizes above are estimates in days. "Small / medium / large" compare the
  items with each other.
- The description of how other wallets sync is a summary of published
  designs, not a study of their code.
