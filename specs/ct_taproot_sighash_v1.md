# CT Taproot Sighash v1

This document freezes the consensus-critical Taproot key-path sighash rule for
Dinero confidential prevouts.

## Scope

- Transparent-only Taproot spends remain on the normal BIP341-style
  `TaggedHash("TapSighash", preimage)` path.
- Any Taproot spend whose prevout set contains at least one confidential input
  switches to the Dinero-specific CT-aware variant described here.

## Domain Separation

The CT-aware variant is domain-separated in two places:

1. The confidential prevout descriptor extension is hashed as:
   - `TaggedHash("dinero/ct-prevouts/v1", extension_payload)`
2. The final Taproot sighash for any spend with at least one confidential
   prevout is hashed as:
   - `TaggedHash("dinero/sighash/v1", preimage_with_extension)`

This ensures transparent-only, mixed, and all-confidential Taproot spends do
not share the plain BIP341 `TapSighash` domain.

## Base Taproot Preimage

The CT-aware variant starts from the normal Taproot key-path preimage shape used
by Dinero for transparent spends. The same field order, endianness, and
ANYONECANPAY / SINGLE / NONE semantics are retained.

The only consensus changes are:

1. Confidential prevouts serialize as amount `0` in the `sha_amounts`
   commitment slots, because validators do not have a plaintext amount for
   those prevouts.
2. A confidential prevout extension is appended to the end of the Taproot
   preimage before the final tagged hash.

Transparent prevouts keep their normal plaintext amount in `sha_amounts`.

## Confidential Prevout Extension

`extension_payload` serializes exactly as:

1. `version` as a single byte
   - current value: `0x01`
2. `anyonecanpay` as a single byte
   - `0x00` when `ANYONECANPAY` is not set
   - `0x01` when `ANYONECANPAY` is set
3. `input_count` as CompactSize / varint
   - if `ANYONECANPAY`, this value is always `1`
   - otherwise this is the full prevout count
4. One prevout descriptor per committed input, in input order

Each prevout descriptor serializes as:

1. `type_flag` as a single byte
   - `0x00` = transparent prevout
   - `0x01` = confidential prevout
2. `commitment_len` as CompactSize / varint
3. `commitment_bytes`

Descriptor rules:

- Transparent prevout:
  - `type_flag = 0x00`
  - `commitment_len = 0`
  - no commitment bytes follow
- Confidential prevout:
  - `type_flag = 0x01`
  - `commitment_len = len(prevout.commitment)`
  - `commitment_bytes = prevout.commitment`

## Mixed Prevout Rule

Mixed-input Taproot spends are valid and unambiguous.

For each prevout independently:

- transparent prevout commits:
  - type tag `transparent`
  - plaintext amount in the base Taproot `sha_amounts`
  - scriptPubKey in the normal Taproot `sha_scriptpubkeys`
- confidential prevout commits:
  - type tag `confidential`
  - commitment bytes in the CT extension
  - amount `0` in the base Taproot `sha_amounts`
  - scriptPubKey in the normal Taproot `sha_scriptpubkeys`

Changing a prevout from transparent to confidential, or changing the committed
confidential commitment bytes, must change the final sighash.

## ANYONECANPAY

When `ANYONECANPAY` is set:

- the base Taproot preimage still commits only the current input
- the CT extension also commits only the current input
- `input_count` in the extension is `1`

## Consensus Notes

- The CT extension supplements the Taproot preimage. It does not replace the
  existing Taproot commitments for version, locktime, outputs, scriptPubKeys,
  annex, leaf hash, or sighash type.
- Taproot key-path validation must fail closed if the verifier does not have
  the complete prevout context for every input:
  - amount
  - scriptPubKey
  - confidential flag
  - commitment bytes
  No implementation may synthesize empty scripts, zero amounts, or transparent
  placeholders for missing prevout metadata.
- Any future change to the extension format requires a new version byte and a
  new tagged-hash domain.

## Reference Test Vectors

The implementation is frozen against these reference hashes in
`tests/test_taproot_consensus.cpp`:

### Mixed transparent + confidential prevouts, SIGHASH_DEFAULT

- input set:
  - prevout 0 transparent, amount `5000`
  - prevout 1 confidential, commitment bytes `08 || 0x22 * 32`
- scriptPubKeys:
  - both Taproot key-path `51 20 || xonly_pubkey`
- output set:
  - one Taproot output of amount `7777`
- signing input:
  - input index `0`
- resulting sighash:
  - `cb0c003b8a7ddc4375d092c107a98397ebf9c6222104a4a4819bb2ff62325cc0`

Transparent-only comparison vector for the same transaction shape:

- prevout flags:
  - both transparent
- resulting sighash:
  - `79fd6401a71b6b352d1b04aeacc3d8a486b53a2713315b5058904bf6dee6c4f8`

### Mixed transparent + confidential prevouts, SIGHASH_ANYONECANPAY

- input set:
  - prevout 0 transparent, amount `5000`
  - prevout 1 confidential, commitment bytes `08 || 0x33 * 32`
- output set:
  - one Taproot output of amount `8888`
- signing input:
  - input index `1`
- sighash type:
  - `0x80` (`SIGHASH_ANYONECANPAY`)
- resulting sighash:
  - `54015a9e2fb86b813f26958378df8a10da4ddf39a4dba4c125168cac27db1b18`
