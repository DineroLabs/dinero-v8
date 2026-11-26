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
Not something this project can do for itself. Scope to offer a reviewer: the
integration (bundle decoding, admission checks, the commitment-tree frontier,
note encryption use, key derivation paths), not the upstream crate.

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

## 3. A fee rule that prices spam

### Why
In mid-2022 someone filled Zcash blocks with shielded outputs at almost no
cost. Nodes coped; wallets did not, because every wallet has to try to
decrypt every output. Zcash answered with a fee proportional to the number
of actions (ZIP 317). Dinero today takes the fee as a number the wallet or
user supplies (`fee_una` in `wallet.orchard.queueshield` / `queuespend`).
What the mempool and the miner require of an Orchard transaction's fee was
not established for this note and is the first thing the spec must pin down.

### Build, in two stages
- Stage A, policy only (no fork): the wallet computes a default fee from the
  action count; the mempool refuses to relay, and the block assembler
  declines to include, an Orchard transaction paying less than
  `per_action × max(actions, floor)`. Old wallets that underpay see a clear
  refusal with the required amount.
- Stage B, only if stage A proves insufficient: the same rule as a consensus
  rule at an activation height.

Decisions for the owner: the per-action amount and the floor; whether the
phone app applies the same default at once.

Size: medium. Stage A touches mempool admission, block assembly, the wallet
default and both apps' fee fields. It is network policy, so it needs the
separation-gate review that consensus-adjacent changes get.

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
| Next minor release | 1b step 2; 2 watch-only import; 3 stage A spec agreed and built |
| v8.3 line | 5a design and baseline, then compact scan data; 1b step 3 (stock crate) |
| Decide, then schedule | 4 (only if bundling is wanted); 5b (product decision); 1c (outside review) |

## Decisions needed from the owner

1. Viewing keys: full only, or full and incoming-only?
2. Fee rule: per-action amount and floor; phones at the same time?
3. Unified addresses: wanted, or drop the item?
4. Phones: stay embedded nodes, light mode as an option, or light by default?
5. Outside review: is there budget and a reviewer in mind?

## What this note does not claim

- The fee handling of Orchard transactions in the mempool and miner was not
  traced; item 3 starts by establishing it.
- No sizes above are estimates in days. "Small / medium / large" compare the
  items with each other.
- The description of how other wallets sync is a summary of published
  designs, not a study of their code.
