# Covenant profile v1 wallet construction and recovery

Status: implemented and exercised on regtest. The mainnet consensus profile is
scheduled for block 80,000, but these construction RPCs remain regtest-only
until a separate production-wallet release enables them.

The wallet layer constructs and recovers the transparent CTV and CCV forms
defined by `../consensus/DINERO_COVENANT_PROFILE_V1.md`. It does not introduce
new consensus rules and it does not authorize an activation height.

## Safety boundary

- Every handler fails closed unless the selected chain is regtest.
- Mainnet CTV/CCV consensus activation is block 80,000; testnet remains
  dormant. The RPC chain guard remains independent and fail-closed.
- Descriptors contain public construction data only. They contain no seed,
  private key, or signing capability.
- CTV outputs use the BIP341 recommended NUMS internal key, so the wallet does
  not possess a key-path escape.
- CCV internal keys and successor outputs are re-derived from the authenticated
  contract state and fixed profile code.
- The default CCV artifact is owner-authorized. Its tapscript is
  `OP_CHECKCONTRACTVERIFY <owner-xonly-key> OP_CHECKSIG`. The CCV operation
  first consumes and validates the old/new state pair, then the BIP340
  signature authorizes the final transaction. The owner signature therefore
  commits to every input, prevout amount/script, sequence, output, and locktime.
- The original permissionless profile remains recoverable for compatibility.
  Its tapscript is `OP_CHECKCONTRACTVERIFY OP_TRUE`, and creation/advance still
  require explicit `permissionless: true` acknowledgement.
- Spend construction fails before the relevant activation height. Before
  activation, CTV is NOP4 and CCV is an `OP_SUCCESS` slot, so constructing a
  nominal spend would not enforce the covenant.
- Construction is not validation. Unit and integration tests submit the
  resulting wire transaction through the canonical spend validator, mempool,
  mining, block connection, and reorg paths.

## Recovery descriptor

The textual form starts with `dncov1:` and hex-encodes:

1. magic `DNCOV1`;
2. profile version and type;
3. the type-specific canonical body; and
4. a SHA-256 checksum over the preceding bytes.

The descriptor id is SHA-256 of the complete textual descriptor.

For CTV, the body stores the covenant input index and the canonical
witnessless transaction template with zero prevouts. Recovery recomputes the
BIP119 template hash, tapscript, Taproot output, and control block. Spend
construction may replace only the uncommitted prevout identifiers.

For permissionless CCV (descriptor type 2), the body stores the counter and
state data. For owner CCV (descriptor type 3), it additionally stores the
32-byte x-only owner public key and its public wallet key origin. Recovery
recomputes the state hash, code hash, state-derived internal key, Taproot
output, and control block. A transition increments the counter, preserves the
exact transparent covenant value and owner, and puts the unique successor at
output zero. Additional inputs and outputs may fund fees; their witnesses
remain for the normal wallet signer.

The key origin contains no private material. On advance, the wallet re-derives
the private key from that origin and refuses to sign unless its BIP340 x-only
public key exactly matches the descriptor. A restored seed plus descriptor is
therefore sufficient to recover signing capability without placing a secret
inside the descriptor.

Malformed, non-canonical, wrong-profile, unknown-version, and
checksum-corrupted descriptors are rejected.

## Wallet persistence

Schema version 26 adds `covenant_descriptors`. A descriptor, its derived
scriptPubKey, label, and optional CCV parent id are committed in the same
SQLite transaction as the watch-script registration.

Watch paths use:

```text
m/covenant/1/<descriptor-id>
```

This is a watch-only namespace, not a BIP32 derivation path.
`deriveKeyForScriptPubKey` refuses it rather than inventing key ownership.
Exact re-import is idempotent; descriptor-id or script collisions fail closed.

Persist the current CCV descriptor and every constructed successor needed for
recovery. The parent id records lineage but does not replace validation of the
descriptor itself.

## RPC surface

All responses use `din.wallet.covenant.profile.v1`.

- `wallet.covenant.ctvcreate` constructs a CTV plan from exact `value_una`
  outputs, committed input sequences, input index, version, and locktime.
- `wallet.covenant.ctvspend` recovers the plan and supplies its prevout
  identifiers.
- `wallet.covenant.ccvcreate` allocates a fresh wallet Taproot key and constructs
  an owner-authorized initial state by default. `permissionless: true` selects
  the legacy unowned profile explicitly.
- `wallet.covenant.ccvadvance` re-derives and verifies the owner key, signs the
  final covenant input, and leaves ordinary fee inputs unsigned. Each fee input
  supplies `prevout_value_una` plus `script_pubkey` (or `address`) because the
  BIP341 signature commits to all prevouts.
- `wallet.covenant.import` validates, re-derives, and tracks a descriptor.
- `wallet.covenant.inspect` validates and displays a descriptor without
  storing it.
- `wallet.covenant.list` lists durable recovery records.

Creation RPCs track only when requested. Callers should save the returned
descriptor independently before funding its scriptPubKey.

Amounts created by these RPCs use unsigned integer `value_una` fields. This
avoids floating-point ambiguity at the covenant construction boundary.

Owner authorization removes the known permissionless-wallet blocker. It does
not by itself authorize shipping mainnet construction: the production-wallet
guard, release-candidate activation boundary, reorg, performance, and
independent consensus-review gates remain separate requirements.

## Signing and recovery workflow

1. Construct and independently back up the returned descriptor.
2. Track or import it before funding the returned scriptPubKey.
3. Build the covenant spend/transition.
4. For extra wallet-owned fee inputs, pass all prevout metadata to
   `wallet.signrawtransaction`.
5. Require `complete: true`. The signer preserves the existing covenant
   witness, signs other owned inputs, then runs every input through canonical
   validation at the next active-chain height.
6. Broadcast through the normal transaction-ingress RPC.
7. After restart, list and inspect stored descriptors and compare their
   re-derived scriptPubKeys with the independent backup.

Reorg reconciliation restores a disconnected valid transaction to the local
mempool with relay disabled under existing policy because it is not a newly
received transaction. An operator or wallet may explicitly rebroadcast the
same signed bytes; the lifecycle test proves independent admission, relay, and
reconfirmation after that rebroadcast.

## Executable evidence

- `CovenantProfileWallet` verifies deterministic recovery, canonical CTV/CCV
  validation, owner-key matching, transaction/signature mutations,
  wire-serialization round trips, and descriptor failures.
- `CovenantWalletRecovery` verifies schema migration, restart recovery,
  watch-only key behavior, idempotency, lineage, and collision rollback.
- `CovenantWalletMultinodeLifecycle` runs two isolated regtest daemons and
  proves owner-key allocation, funding, covenant and fee-input signing, relay,
  mining, wallet restart, descriptor/key recovery, reorg revalidation,
  explicit rebroadcast, and reconfirmation. It also retains an explicitly
  acknowledged permissionless compatibility check.

These tests are necessary evidence for external review. They are not a
substitute for independent consensus review or a coordinated activation plan.
