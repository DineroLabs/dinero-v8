# Covenant profile v1 wallet construction and recovery

Status: implemented for regtest review; not approved for testnet or mainnet.

The wallet layer constructs and recovers the transparent CTV and CCV forms
defined by `../consensus/DINERO_COVENANT_PROFILE_V1.md`. It does not introduce
new consensus rules and it does not authorize an activation height.

## Safety boundary

- Every handler fails closed unless the selected chain is regtest.
- Mainnet and testnet CTV/CCV activation heights remain `UINT32_MAX`.
- Descriptors contain public construction data only. They contain no seed,
  private key, or signing capability.
- CTV outputs use the BIP341 recommended NUMS internal key, so the wallet does
  not possess a key-path escape.
- CCV internal keys and successor outputs are re-derived from the authenticated
  contract state and fixed profile-v1 code.
- The v1 CCV wallet artifact is deliberately permissionless: its tapscript is
  `OP_CHECKCONTRACTVERIFY OP_TRUE` and contains no signature check. Any party
  can fund the fee and choose the next `data`. The RPC requires the caller to
  set `permissionless: true`; this artifact is lifecycle evidence, not an
  owner-controlled contract.
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

For CCV, the body stores the counter and state data. Recovery recomputes the
state hash, fixed code hash, state-derived internal key, Taproot output, and
control block. A transition increments the counter, preserves the exact
transparent covenant value, and puts the unique successor at output zero.
Additional inputs and outputs may fund fees; their witnesses remain for the
normal wallet signer.

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
- `wallet.covenant.ccvcreate` constructs an initial permissionless CCV state
  after explicit `permissionless: true` acknowledgement.
- `wallet.covenant.ccvadvance` constructs its permissionless next state and
  leaves ordinary fee inputs unsigned; it requires the same acknowledgement.
- `wallet.covenant.import` validates, re-derives, and tracks a descriptor.
- `wallet.covenant.inspect` validates and displays a descriptor without
  storing it.
- `wallet.covenant.list` lists durable recovery records.

Creation RPCs track only when requested. Callers should save the returned
descriptor independently before funding its scriptPubKey.

Amounts created by these RPCs use unsigned integer `value_una` fields. This
avoids floating-point ambiguity at the covenant construction boundary.

An owner-authorized CCV profile requires authenticated script logic (for
example, a reviewed tapscript signature policy), corresponding wallet key and
recovery semantics, and new adversarial tests. It is a production blocker, not
an implied property of profile v1.

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
  validation, wire-serialization round trips, mutations, and descriptor
  failures.
- `CovenantWalletRecovery` verifies schema migration, restart recovery,
  watch-only key behavior, idempotency, lineage, and collision rollback.
- `CovenantWalletMultinodeLifecycle` runs two isolated regtest daemons and
  proves funding, relay, mining, wallet restart, recovery, reorg revalidation,
  explicit rebroadcast, and reconfirmation.

These tests are necessary evidence for external review. They are not a
substitute for independent consensus review or a coordinated activation plan.
