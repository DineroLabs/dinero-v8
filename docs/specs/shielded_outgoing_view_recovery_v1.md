# Shielded outgoing-view recovery v1

**Status:** dormant production candidate plus independently generated vectors.
The C++ wallet serializer, parser, scanner, sender-history store and regtest
lifecycle rehearsal implement this document. Mainnet and testnet emission
remain dormant at `UINT32_MAX`; no consensus rule or outgoing envelope is
activated until a separately reviewed rollout selects real heights. The
recipient-authority branch does deliberately replace incomplete 43/75-byte
wallet addresses with the 107-byte form before activation; mainnet fund-moving
shielded RPCs remain locked while that migration is reviewed.

**Wire envelope:** encrypted-note envelope version `0x03`.

**Hard dependency:** envelope v3 MUST NOT activate before recipient-bound
shielded spend authority. Dinero's legacy note scheme derives spend authority
from `rcm`, and outgoing recovery necessarily recovers the recipient plaintext,
including `rcm`. On a legacy note that would turn an outgoing viewing key into
a spending capability. Post-spend-authority notes instead commit to
`pk_d_spend = s*G`, where
`s = even_y_normalize(ask + Poseidon(ak, d))`, so neither `ovk`, `ivk`, nor
recovered `rcm` is enough to spend.

This dependency is independent of the snapshot state-commitment activation
work. The specification and vectors can be reviewed before that work finishes;
production emission must wait for the spend-authority cutover and its review.

## 1. Goals and non-goals

An account's outgoing viewing key (`ovk`) should let an authorized wallet or
auditor reconstruct outputs created by that account:

- recipient address components;
- amount and memo;
- note randomness needed to recompute the note commitment; and
- the normalized ephemeral secret needed to decrypt the existing recipient
  ciphertext.

It must not reveal the recipient's spend scalar, authorize a spend, change the
consensus meaning of an output, or claim that an output was mined or accepted.
Chain context still establishes confirmation and canonical-chain membership.

Outgoing recovery is cooperative sender metadata. A sender can omit or corrupt
it, so possession of `ovk` cannot prove that an account's disclosed outgoing
history is complete. Consensus cannot verify correct encryption to a secret
`ovk`.

## 2. Existing account recovery key

This design adds no new outgoing-recovery key derivation. The account-level
outgoing key remains the existing 32-byte value:

```
ovk = Poseidon(sk, PAD32("DIN/v7/shielded/ovk"))
```

`ovk` is symmetric key material, not a scalar. The per-output recovery key is
called `ock` below. The recipient-authority format separately adds `nvk`, so a
full viewing package is `(ak, nk, nvk, ovk)`: it can authenticate ownership,
derive nullifiers, and recover sent notes, but cannot derive spend scalar `s`.

## 3. Activation policy

Two independently named heights are required:

- `shielded_spend_auth_activation_height`; and
- a future `shielded_outgoing_recovery_activation_height`.

They MUST NOT be aliases and MUST use explicit dormant-sentinel handling:

```
Active(height, activation) =
    activation != UINT32_MAX && height >= activation
```

Configuration is invalid unless the outgoing height is dormant or both of the
following hold:

```
spend_auth_height != UINT32_MAX
outgoing_height >= spend_auth_height
```

The exact `height == UINT32_MAX == activation` edge remains dormant. A bare
`height >= activation` check is forbidden because it activates at that
attacker-representable degenerate point.

Wallets MUST emit v3 only when both predicates are active. Wallet recovery MUST
return `SpendAuthorityRequired` or `OutgoingRecoveryInactive`, as appropriate,
when a v3 envelope appears earlier. The consensus parser continues treating
`encrypted_note` as opaque until a separate activation proposal explicitly
changes that policy.

Construction keys activation to the current active-tip height, not a predicted
next-block height: a wallet waits until both rules are already active before it
creates v3. Confirmed recovery uses the output's actual block height. An
unconfirmed output has no canonical height; it may be recovered provisionally
only when both rules are active at the wallet's current active tip, and that
result MUST be recomputed when the transaction confirms or a reorg changes its
height. A provisional result must never be cached as a confirmed recovery.

## 4. Point and integer conventions

- secp256k1 x-only points are 32-byte big-endian x coordinates lifted to the
  unique even-y point.
- Scalars are 32-byte big-endian canonical values in `[1, n-1]`.
- Amounts in the embedded recipient plaintext remain unsigned 64-bit
  little-endian.
- All byte strings below are concatenated without implicit length prefixes;
  every component has a fixed length.
- ChaCha20-Poly1305 uses the IETF 12-byte nonce, fixed to twelve zero bytes.
  This is safe only because `ock` is independently derived from the output's
  commitment, value commitment, ephemeral key, and recipient-ciphertext hash.

## 5. Public recovery context and per-output key

For an output with authenticated note commitment `cm`, Pedersen value
commitment `cv`, and recipient ephemeral key `epk`:

```
public_context = cm[32] || cv[33] || epk[32]                 // 97 bytes
recipient_hash = SHA256(recipient_encrypted_note[611])

salt_preimage = ASCII("DIN/v8/shielded/outgoing/salt/v1") ||
                public_context || recipient_hash
salt          = SHA256(salt_preimage)

ock = HKDF-SHA256(
        salt = salt,
        ikm  = ovk[32],
        info = ASCII("DIN/v8/shielded/outgoing/key/v1"),
        L    = 32)
```

Changing `cm`, `cv`, `epk`, or any recipient-ciphertext byte changes the key.
The value commitment is included even though recovery does not open it: it
binds the metadata to the exact output and prevents transplantation between
otherwise similar outputs. Hashing the full recipient ciphertext into the key
derivation also ensures the fixed nonce cannot reuse a ChaCha20 keystream when
recipient ciphertexts differ.

## 6. Recipient ciphertext retained verbatim

The current recipient ciphertext is preserved byte-for-byte:

```
recipient_encrypted_note =
    epk[32] || recipient_ciphertext[563] || recipient_tag[16] // 611 bytes
```

Its decrypted plaintext remains:

```
d[11] || value_le64[8] || rcm[32] || memo[512]               // 563 bytes
```

Embedding this ciphertext unchanged avoids two independently evolving note
formats. It does not make v3 readable by old wallets: current wallets require
the entire encrypted-note field to be exactly 611 bytes and will ignore the
757-byte envelope. The note still exists on chain, but an old recipient wallet
will not display or spend it until the wallet upgrades and rescans. Coordinated
wallet readiness is therefore an activation requirement, not optional polish.

## 7. Outgoing plaintext and ciphertext

The outgoing plaintext deliberately does not contain `rcm` directly:

```
outgoing_plaintext =
    plaintext_version[1] ||
    pk_d_enc[32]          ||
    pk_d_spend[32]        ||
    nfk_commitment[32]    ||
    esk_normalized[32]                                     // 129 bytes
```

The literal `plaintext_version` is `0x02`. `pk_d_enc` and `pk_d_spend` are
x-only even-y points. `esk_normalized` is the unique scalar for which the full
point `esk * P_d` has even y and its x coordinate equals `epk`. The alternate
scalar `n - esk` is non-canonical and must be rejected even though it has the
same x coordinate.

`nfk_commitment = Poseidon(nfk, DST32("DIN/v7/shielded/nfkey/v1"))`, where
`nfk = Poseidon(nvk, d)`. It lets a full viewer authenticate the note and track
its eventual nullifier without exposing either `nvk` or spend authority.
Together the three public fields reconstruct the full 107-byte address payload.
The note commits to `Poseidon(pk_d_spend, nfk_commitment)` as its ownership key.

The associated data is:

```
aad = ASCII("DIN/v8/shielded/outgoing/aad/v1") ||
      0x03 ||
      public_context[97] ||
      recipient_encrypted_note[611]
```

Encryption is:

```
(outgoing_ciphertext[129], outgoing_tag[16]) =
    ChaCha20-Poly1305-Encrypt(
        key = ock,
        nonce = 0x000000000000000000000000,
        aad = aad,
        plaintext = outgoing_plaintext)
```

The complete encrypted-note field is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | envelope version `0x03` |
| 1 | 611 | unchanged recipient encrypted note |
| 612 | 129 | outgoing ciphertext |
| 741 | 16 | outgoing authentication tag |
| **total** | **757** | |

There is no trailing-data tolerance and no alternate compact-size encoding
inside the envelope. The surrounding transaction vector continues to carry its
existing canonical CompactSize length of 757.
That length prefix is `fd f5 02`.

## 8. Sender construction

For every v3-eligible output constructed after the wallet has observed both
rules active at its current tip, the sender:

1. validates both recipient public keys and the non-zero nullifier-key
   commitment from the 107-byte address;
2. independently samples fresh `rcm`, `esk`, and value-commitment blinding
   randomness for this output; intentional reuse is forbidden, and a detected
   repeat of `(cm, cv, epk, recipient_hash)` under the same `ovk` must be
   discarded and regenerated;
3. constructs the existing 563-byte recipient plaintext and 611-byte
   recipient ciphertext;
4. constructs the authenticated note commitment using
   `Poseidon(pk_d_spend, nfk_commitment)`, not the legacy `rcm`-derived note key;
5. constructs a valid Pedersen value commitment `cv`;
6. derives `ock` from `ovk` and the public context;
7. encrypts the 129-byte outgoing plaintext with the exact AAD above; and
8. emits the 757-byte v3 envelope.

The wallet MUST erase `esk` and `ock` with its other transaction-construction
secrets after use. It MUST NOT emit v3 when recipient-bound spend authority is
inactive, even if an RPC or local configuration asks it to.

## 9. Recovery algorithm

Given `ovk`, the public output `(cm, cv)`, envelope bytes, output height, and
both activation heights:

1. Classify a 611-byte input as `LegacyNoOutgoingRecovery`.
2. Require exactly 757 bytes and envelope version `0x03`.
3. Apply the two activation predicates and ordering rule.
4. Extract the embedded recipient ciphertext and `epk`.
5. Derive `ock`, reconstruct the AAD, and authenticate/decrypt the outgoing
   ciphertext.
6. Require plaintext version `0x02`; validate both x-only points, the non-zero
   nullifier-key commitment, and the canonical non-zero `esk` scalar.
7. Compute `shared = even_y(esk * pk_d_enc)` and derive the existing recipient
   note key. Authenticate/decrypt the embedded recipient ciphertext.
8. Recover `d`, value, `rcm`, and memo. Compute `P_d = HashToPoint(d)` and
   require the unmodified point `esk * P_d` to have even y and x coordinate
   `epk`.
9. Recompute the post-spend-authority note commitment from
   `(d, Poseidon(pk_d_spend, nfk_commitment), value, rcm)` and require equality
   with `cm`.
10. Return the 107-byte recipient payload
    `d || pk_d_enc || pk_d_spend || nfk_commitment`, amount, memo, and
    commitment inputs.
    Address text is then encoded with the HRP selected by chain parameters; the
    recovery ciphertext does not carry a network identifier.

Steps 7–9 are essential. AEAD success alone proves only that someone knowing
`ovk` constructed the outgoing metadata. It does not prove those bytes describe
the recipient ciphertext or the on-chain commitment.

## 10. Failure classes

Wallet implementations MUST preserve these semantic distinctions:

| Verdict | Meaning and handling |
|---|---|
| `Ok` | Recovery and all consistency checks succeeded. |
| `LegacyNoOutgoingRecovery` | Valid-sized legacy envelope; incoming scan may continue. |
| `InvalidLength` | Neither the exact legacy nor v3 length. Never reinterpret a prefix. |
| `UnsupportedEnvelopeVersion` | Exact v3 length but unknown version. Ignore for viewing. |
| `SpendAuthorityRequired` | v3 appeared before recipient-bound spend authority. |
| `OutgoingRecoveryInactive` | spend authority is active but outgoing recovery is not. |
| `NotForViewerOrTampered` | Outgoing AEAD failed. Wrong `ovk` and tampering are intentionally indistinguishable. This is a quiet scan miss, not a per-output operator error. |
| `UnsupportedPlaintextVersion` | Authenticated outgoing plaintext has an unknown version. |
| `InvalidEncryptionKey` | Authenticated `pk_d_enc` is not a canonical x-only point. |
| `InvalidSpendKey` | Authenticated `pk_d_spend` is not a canonical x-only point. |
| `InvalidNullifierKeyCommitment` | Authenticated nullifier-key commitment is zero or not a canonical scalar encoding. |
| `InvalidEphemeralSecret` | `esk` is zero or outside the scalar field. |
| `RecipientAuthenticationFailed` | Outgoing metadata authenticated, but the embedded recipient ciphertext did not. |
| `EphemeralKeyMismatch` | Both layers authenticated, but recovered `esk` does not produce the canonical even-y `epk` for recovered `d`. This includes the non-canonical `n - esk` encoding. |
| `CommitmentMismatch` | Recovered note data does not reproduce on-chain `cm`. |

Failure classes are wallet-recovery results, not new block-validation rules.
Malformed or unrecoverable metadata does not by itself invalidate a block under
this design-stage specification.

## 11. Compatibility matrix

| Output | Old wallet | Upgraded wallet before activation | Upgraded wallet after both activations |
|---|---|---|---|
| 611-byte legacy v1 | incoming scan | incoming scan; no outgoing recovery | incoming scan; no outgoing recovery |
| 757-byte v3 | undiscovered until upgrade and rescan | refuses to emit; classifies early receipt | incoming scan and outgoing recovery |
| unknown future version | ignored | ignored | ignored, never parsed as legacy/v3 |

There is no retroactive recovery for legacy outputs. Wallet databases should
store the envelope version and recovery verdict so later rescans are stable,
but must not cache `NotForViewerOrTampered` across an imported or changed `ovk`.

An encrypted software wallet keeps viewing authority in memory across
`wallet.lock`, allowing confirmation and reorg scanning without spend
authority. It does not persist raw viewing keys in plaintext: after a process
restart the operator must unlock once to repopulate the viewing cache, then may
immediately re-lock. Already recovered outgoing history remains readable while
locked and across restart.

Recipient-bound note rows persist a zero spend-key placeholder, including notes
created or discovered while unlocked. Every spend re-derives its scalar from
the unlocked seed and checks the note's nullifier key and ownership commitment.
Legacy insertion overloads refuse the Auth scheme so they cannot copy the spend
scalar into the nullifier-view column. Opening a development database clears
previously cached Auth spend keys from live rows; this does not erase older
backups or guarantee removal of historical SQLite pages. Such development
databases must not be treated as having protected those keys at rest.

## 12. Security and privacy properties

- `ovk` reveals outgoing recipient, amount, memo, `rcm`, and transaction-linking
  information. It should be treated as sensitive audit authority.
- The outgoing plaintext omits `rcm` as defense in depth, but successful
  recovery obtains it from the recipient plaintext. The activation dependency,
  not this omission, is what prevents spending legacy-style notes.
- `ovk` does not reveal recipient spend scalar `s` or account spending key
  `ask` under the post-spend-authority construction.
- A sender can create unrecoverable outgoing metadata. This scheme provides
  recovery, not cryptographic proof of complete disclosure.
- Wrong-key and tamper failures are deliberately indistinguishable to avoid an
  `ovk` membership oracle.
- Reusing the fixed nonce with a context-independent key is forbidden. Any
  future context change requires new vectors and a version change. The
  commitment, value commitment, ephemeral key, and recipient-ciphertext hash
  in `salt_preimage` are load-bearing for this rule. Sender-side freshness of
  `rcm`, `esk`, and value-commitment blinding randomness is also mandatory;
  the derivation is not permission to intentionally repeat an output context.
- Wallet logs must not print plaintext, `ovk`, `ock`, `esk`, `rcm`, or memo on
  recovery failure.

## 13. Independent oracle and vectors

The standard-library oracle is
`scripts/generate_shielded_outgoing_view_vectors.py`; its checked output is
`tests/vectors/shielded_outgoing_view_v3_external.json`.

The oracle imports only the independent primitive implementation introduced by
the shielded-protocol vector package. It does not import Dinero production code
or link a crypto library. It independently derives the account fixture,
recipient ciphertext, authenticated note commitment, real Pedersen value
commitment, outgoing key/AAD/ciphertext, and full recovery result.

Its self-test exercises every named failure class, including authenticated
mutants that reach the deep `EphemeralKeyMismatch`,
`RecipientAuthenticationFailed`, and `CommitmentMismatch` branches rather than
failing at the outer tag. It also mutation-pins:

- exact dormant-sentinel behavior;
- activation ordering;
- the 611/757-byte split;
- rejection of unknown versions and trailing/truncated bytes;
- wrong-`ovk`/tamper indistinguishability; and
- absence of `rcm`, the legacy `rcm`-derived spend key, and the recipient spend
  scalar from the direct outgoing plaintext;
- inequality of the legacy and recipient-bound commitments for the same note;
- independent key changes when only the note commitment, value commitment,
  ephemeral key, or embedded recipient ciphertext changes; and
- exact coverage of every declared recovery verdict, including canonical
  `Ok`, so adding an untested verdict makes the oracle fail.

The vectors are version 1 of the oracle artifact and describe envelope version
3. Changing any domain, byte order, component, failure meaning, or activation
dependency requires a new vector-artifact version. The production C++
conformance test consumes the checked-in constants directly and reconstructs
the envelope and recovery result without using the Python oracle at runtime.
This proves agreement between the independent oracle and the C++
implementation; it does not replace cryptographic review.

## 14. Production landing gates

The dormant implementation closes the mechanical part of the production
package. Activation remains forbidden until every row below is closed on the
exact activation candidate:

| Gate | Candidate status | Required before activation |
|---|---|---|
| Recipient-bound spend authority | Implemented; mainnet/testnet dormant | Activate or schedule no later than envelope v3. |
| One activation decision | Implemented with explicit sentinel-aware predicates | Re-check the selected network heights. |
| C++ byte conformance | Implemented against the independent JSON constants | Re-run on every release architecture. |
| Sender construction and recovery | Implemented for shield, transfer, recipient output and authenticated change | Independent cryptographic review. |
| Wallet lifecycle | Unit coverage plus a real regtest confirmation/restart/reorg/reconnect rehearsal | Exact-head CI and release-candidate rerun. |
| Hardware-wallet boundary | Host capability negotiation and fail-closed emulator coverage implemented | Real firmware support and physical-device smoke testing. |
| Legacy compatibility | legacy 611-byte notes retained before activation; because construction uses the current tip, ordinary v3 emission starts while building on an already-active block (normally the block after the activation block), not speculatively for the activation block itself | Coordinated old-wallet cutoff and release communication. |
| Mutation controls | Parser/deep-consistency vectors and lifecycle state transitions are pinned | Independent reviewer repeats the controls. |
| Privacy/security review | Not performed by this implementation branch | Independent cryptographic and wallet-privacy review. |

Watch-only sender recovery is represented by the account-scoped `ovk` and the
separate outgoing-history database. A public import/export RPC for `ovk` is
deliberately not exposed in this candidate: key disclosure is a privacy event
and needs its own authenticated backup and UX policy. Hardware devices likewise
advertise the capability explicitly and return only `ovk`; unsupported or
refusing firmware cannot fall back to a software seed.

The release candidate MUST test at least one physical device for every firmware
advertised as supporting shielded outgoing view. The checklist is: capability
is absent on old firmware; dormant rules cause no prompt; active rules request
the selected account only; rejection aborts construction; returned all-zero
material is rejected; unplug/reconnect/restart does not invent sender history;
and a rescan with the same device-derived `ovk` reproduces the software oracle
result. Any future structured device response needs its own canonical parser
and malformed-material tests; the current host boundary returns a fixed
32-byte symmetric key, for which every nonzero byte string is representable.

Gate E's snapshot binding may proceed independently. If snapshot state later
stores wallet recovery metadata, that is a new scope requiring its own version
and commitment analysis; this specification makes no such change.
