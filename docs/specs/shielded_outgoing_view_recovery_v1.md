# Shielded outgoing-view recovery v1

**Status:** prospective design and independently generated vectors. No
production serializer, parser, wallet scanner, activation height, or consensus
rule is changed by this specification.

**Wire envelope:** encrypted-note envelope version `0x02`.

**Hard dependency:** envelope v2 MUST NOT activate before recipient-bound
shielded spend authority. Dinero's legacy note scheme derives spend authority
from `rcm`, and outgoing recovery necessarily recovers the recipient plaintext,
including `rcm`. On a legacy note that would turn an outgoing viewing key into
a spending capability. Post-spend-authority notes instead commit to
`pk_d_spend = s*G`, where `s = Poseidon(ivk, d)`, so knowing `rcm` is not enough
to spend.

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

This design does not introduce a new account derivation or alter addresses.
The account-level key remains the existing 32-byte value:

```
ovk = Poseidon(sk, PAD32("DIN/v7/shielded/ovk"))
```

`ovk` is symmetric key material, not a scalar. The per-output recovery key is
called `ock` below. Full viewing keys continue to contain `(ak, nk, ovk)`.

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

Wallets MUST emit v2 only when both predicates are active. Wallet recovery MUST
return `SpendAuthorityRequired` or `OutgoingRecoveryInactive`, as appropriate,
when a v2 envelope appears earlier. The consensus parser continues treating
`encrypted_note` as opaque until a separate activation proposal explicitly
changes that policy.

Construction keys activation to the current active-tip height, not a predicted
next-block height: a wallet waits until both rules are already active before it
creates v2. Confirmed recovery uses the output's actual block height. An
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
formats. It does not make v2 readable by old wallets: current wallets require
the entire encrypted-note field to be exactly 611 bytes and will ignore the
725-byte envelope. The note still exists on chain, but an old recipient wallet
will not display or spend it until the wallet upgrades and rescans. Coordinated
wallet readiness is therefore an activation requirement, not optional polish.

## 7. Outgoing plaintext and ciphertext

The outgoing plaintext deliberately does not contain `rcm` directly:

```
outgoing_plaintext =
    plaintext_version[1] ||
    pk_d_enc[32]          ||
    pk_d_spend[32]        ||
    esk_normalized[32]                                      // 97 bytes
```

The literal `plaintext_version` is `0x01`. `pk_d_enc` and `pk_d_spend` are
x-only even-y points. `esk_normalized` is the unique scalar for which the full
point `esk * P_d` has even y and its x coordinate equals `epk`. The alternate
scalar `n - esk` is non-canonical and must be rejected even though it has the
same x coordinate.

`pk_d_spend` is required because Dinero addresses separate encryption and
spend public keys. Its inclusion lets recovery reconstruct the full 75-byte
address payload and recompute the recipient-bound note commitment.

The associated data is:

```
aad = ASCII("DIN/v8/shielded/outgoing/aad/v1") ||
      0x02 ||
      public_context[97] ||
      recipient_encrypted_note[611]
```

Encryption is:

```
(outgoing_ciphertext[97], outgoing_tag[16]) =
    ChaCha20-Poly1305-Encrypt(
        key = ock,
        nonce = 0x000000000000000000000000,
        aad = aad,
        plaintext = outgoing_plaintext)
```

The complete encrypted-note field is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | envelope version `0x02` |
| 1 | 611 | unchanged recipient encrypted note |
| 612 | 97 | outgoing ciphertext |
| 709 | 16 | outgoing authentication tag |
| **total** | **725** | |

There is no trailing-data tolerance and no alternate compact-size encoding
inside the envelope. The surrounding transaction vector continues to carry its
existing canonical CompactSize length of 725.
That length prefix is `fd d5 02`.

## 8. Sender construction

For every post-activation output, the sender:

1. validates both recipient public keys from the 75-byte address;
2. independently samples fresh `rcm`, `esk`, and value-commitment blinding
   randomness for this output; intentional reuse is forbidden, and a detected
   repeat of `(cm, cv, epk, recipient_hash)` under the same `ovk` must be
   discarded and regenerated;
3. constructs the existing 563-byte recipient plaintext and 611-byte
   recipient ciphertext;
4. constructs the authenticated note commitment using `pk_d_spend`, not the
   legacy `rcm`-derived note key;
5. constructs a valid Pedersen value commitment `cv`;
6. derives `ock` from `ovk` and the public context;
7. encrypts the 97-byte outgoing plaintext with the exact AAD above; and
8. emits the 725-byte v2 envelope.

The wallet MUST erase `esk` and `ock` with its other transaction-construction
secrets after use. It MUST NOT emit v2 when recipient-bound spend authority is
inactive, even if an RPC or local configuration asks it to.

## 9. Recovery algorithm

Given `ovk`, the public output `(cm, cv)`, envelope bytes, output height, and
both activation heights:

1. Classify a 611-byte input as `LegacyNoOutgoingRecovery`.
2. Require exactly 725 bytes and envelope version `0x02`.
3. Apply the two activation predicates and ordering rule.
4. Extract the embedded recipient ciphertext and `epk`.
5. Derive `ock`, reconstruct the AAD, and authenticate/decrypt the outgoing
   ciphertext.
6. Require plaintext version `0x01`; validate both x-only points and the
   canonical non-zero `esk` scalar.
7. Compute `shared = even_y(esk * pk_d_enc)` and derive the existing recipient
   note key. Authenticate/decrypt the embedded recipient ciphertext.
8. Recover `d`, value, `rcm`, and memo. Compute `P_d = HashToPoint(d)` and
   require the unmodified point `esk * P_d` to have even y and x coordinate
   `epk`.
9. Recompute the post-spend-authority note commitment from
   `(d, pk_d_spend, value, rcm)` and require equality with `cm`.
10. Return the 75-byte recipient payload
    `d || pk_d_enc || pk_d_spend`, amount, memo, and commitment inputs.
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
| `InvalidLength` | Neither the exact v1 nor v2 length. Never reinterpret a prefix. |
| `UnsupportedEnvelopeVersion` | Exact v2 length but unknown version. Ignore for viewing. |
| `SpendAuthorityRequired` | v2 appeared before recipient-bound spend authority. |
| `OutgoingRecoveryInactive` | spend authority is active but outgoing recovery is not. |
| `NotForViewerOrTampered` | Outgoing AEAD failed. Wrong `ovk` and tampering are intentionally indistinguishable. This is a quiet scan miss, not a per-output operator error. |
| `UnsupportedPlaintextVersion` | Authenticated outgoing plaintext has an unknown version. |
| `InvalidEncryptionKey` | Authenticated `pk_d_enc` is not a canonical x-only point. |
| `InvalidSpendKey` | Authenticated `pk_d_spend` is not a canonical x-only point. |
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
| 725-byte v2 | undiscovered until upgrade and rescan | refuses to emit; classifies early receipt | incoming scan and outgoing recovery |
| unknown future version | ignored | ignored | ignored, never parsed as v1/v2 |

There is no retroactive recovery for legacy outputs. Wallet databases should
store the envelope version and recovery verdict so later rescans are stable,
but must not cache `NotForViewerOrTampered` across an imported or changed `ovk`.

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
`tests/vectors/shielded_outgoing_view_v2_external.json`.

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
- the 611/725-byte split;
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
2. Changing any domain, byte order, component, failure meaning, or activation
dependency requires a new vector-artifact version. A future production
implementation must consume these constants in a separate conformance test;
until then, they prove the design is internally coherent, not that production
implements it.

## 14. Production landing gates

This package is complete as a design/vector artifact. Production work remains
a separate reviewed change and MUST include:

1. recipient-bound spend authority activated or scheduled no later than this
   feature;
2. a single activation predicate shared by construction and scanning;
3. production serializer/parser conformance against every checked-in byte;
4. sender/recipient/`ovk` recovery tests and a sender-cannot-spend test;
5. rescan, reorg, restart, hardware-wallet, and watch-only database coverage;
6. exact behavior for old wallets around the coordinated cutover;
7. mutation proof for each deep consistency check; and
8. independent cryptographic and wallet privacy review.

Gate E's snapshot binding may proceed independently. If snapshot state later
stores wallet recovery metadata, that is a new scope requiring its own version
and commitment analysis; this specification makes no such change.
