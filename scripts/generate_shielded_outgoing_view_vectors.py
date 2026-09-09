#!/usr/bin/env python3
"""Independent prospective Dinero outgoing-view recovery vector oracle.

This is design-stage code, not a production implementation.  It imports only
the standard-library primitive implementation from the independent shielded
protocol vector generator.  It imports no Dinero C++ and links no crypto
library.  The checked-in JSON freezes the proposed byte format and failure
classes before wallet or consensus code is changed.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any, Optional


HERE = pathlib.Path(__file__).resolve().parent
BASE_ORACLE_PATH = HERE / "generate_shielded_protocol_vectors.py"
_SPEC = importlib.util.spec_from_file_location("shielded_protocol_oracle", BASE_ORACLE_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load independent primitive oracle: {BASE_ORACLE_PATH}")
base = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(base)


U32_MAX = 0xFFFFFFFF
LEGACY_ENCRYPTED_NOTE_BYTES = 611
ENVELOPE_VERSION = 3
OUTGOING_PLAINTEXT_VERSION = 2
OUTGOING_PLAINTEXT_BYTES = 129
OUTGOING_CIPHERTEXT_BYTES = OUTGOING_PLAINTEXT_BYTES + 16
V3_ENVELOPE_BYTES = 1 + LEGACY_ENCRYPTED_NOTE_BYTES + OUTGOING_CIPHERTEXT_BYTES

SALT_DOMAIN = b"DIN/v8/shielded/outgoing/salt/v1"
KEY_DOMAIN = b"DIN/v8/shielded/outgoing/key/v1"
AAD_DOMAIN = b"DIN/v8/shielded/outgoing/aad/v1"


class Verdict:
    OK = "Ok"
    LEGACY_NO_OUTGOING_RECOVERY = "LegacyNoOutgoingRecovery"
    INVALID_LENGTH = "InvalidLength"
    UNSUPPORTED_ENVELOPE_VERSION = "UnsupportedEnvelopeVersion"
    SPEND_AUTHORITY_REQUIRED = "SpendAuthorityRequired"
    OUTGOING_RECOVERY_INACTIVE = "OutgoingRecoveryInactive"
    NOT_FOR_VIEWER_OR_TAMPERED = "NotForViewerOrTampered"
    UNSUPPORTED_PLAINTEXT_VERSION = "UnsupportedPlaintextVersion"
    INVALID_ENCRYPTION_KEY = "InvalidEncryptionKey"
    INVALID_SPEND_KEY = "InvalidSpendKey"
    INVALID_NULLIFIER_KEY_COMMITMENT = "InvalidNullifierKeyCommitment"
    INVALID_EPHEMERAL_SECRET = "InvalidEphemeralSecret"
    EPHEMERAL_KEY_MISMATCH = "EphemeralKeyMismatch"
    RECIPIENT_AUTHENTICATION_FAILED = "RecipientAuthenticationFailed"
    COMMITMENT_MISMATCH = "CommitmentMismatch"


@dataclass(frozen=True)
class PublicOutput:
    commitment: bytes
    cv: bytes


@dataclass(frozen=True)
class RecoveryResult:
    verdict: str
    recovered: Optional[dict[str, Any]] = None


def active(height: int, activation_height: int) -> bool:
    """Explicit dormant-sentinel handling, including height == UINT32_MAX."""
    return activation_height != U32_MAX and height >= activation_height


def validate_activation_policy(spend_auth_height: int, outgoing_height: int) -> None:
    if outgoing_height != U32_MAX:
        if spend_auth_height == U32_MAX or outgoing_height < spend_auth_height:
            raise ValueError("outgoing recovery may not activate before spend authority")


def auth_note_commitment(d: bytes, pk_d_spend: bytes,
                         nfk_commitment: bytes, value: int, rcm: bytes) -> bytes:
    if (len(d) != 11 or len(pk_d_spend) != 32 or
            len(nfk_commitment) != 32 or len(rcm) != 32):
        raise ValueError("invalid authenticated-note component size")
    d_packed = d.ljust(32, b"\0")
    ownership_key = base.poseidon(pk_d_spend, nfk_commitment)
    address_key = base.poseidon(d_packed, ownership_key)
    address_binding = base.poseidon(base.dst("DIN/v7/shielded/addr/v1"), address_key)
    return base.poseidon(base.poseidon(address_binding, base.b32(value)), rcm)


def value_commitment(blind: int, value: int) -> bytes:
    """Independent libsecp256k1-zkp-compatible Pedersen serialization."""
    value_generator = base.generator_generate(base.h256(b"DIN/v7/shielded/cv/V/v1"))
    point = base.point_add(base.point_mul(blind), base.point_mul(value, value_generator))
    if point is None:
        raise ValueError("value commitment is the point at infinity")
    is_square = pow(point[1], (base.P - 1) // 2, base.P) == 1
    return bytes([9 ^ int(is_square)]) + base.b32(point[0])


def context_bytes(public: PublicOutput, epk: bytes) -> bytes:
    if len(public.commitment) != 32 or len(public.cv) != 33 or len(epk) != 32:
        raise ValueError("invalid outgoing-recovery public context size")
    return public.commitment + public.cv + epk


def recovery_key(ovk: bytes, public: PublicOutput,
                 recipient_note: bytes) -> tuple[bytes, bytes, bytes, bytes]:
    if len(ovk) != 32:
        raise ValueError("ovk must be 32 bytes")
    if len(recipient_note) != LEGACY_ENCRYPTED_NOTE_BYTES:
        raise ValueError("recipient ciphertext must be the legacy 611-byte encoding")
    epk = recipient_note[:32]
    context = context_bytes(public, epk)
    recipient_hash = base.h256(recipient_note)
    salt_preimage = SALT_DOMAIN + context + recipient_hash
    salt = base.h256(salt_preimage)
    return base.hkdf(salt, ovk, KEY_DOMAIN), salt_preimage, salt, recipient_hash


def outgoing_aad(public: PublicOutput, recipient_note: bytes) -> bytes:
    if len(recipient_note) != LEGACY_ENCRYPTED_NOTE_BYTES:
        raise ValueError("recipient ciphertext must be the legacy 611-byte encoding")
    epk = recipient_note[:32]
    return AAD_DOMAIN + bytes([ENVELOPE_VERSION]) + context_bytes(public, epk) + recipient_note


def outgoing_plaintext(pk_d_enc: bytes, pk_d_spend: bytes,
                       nfk_commitment: bytes, esk: bytes) -> bytes:
    if (len(pk_d_enc) != 32 or len(pk_d_spend) != 32 or
            len(nfk_commitment) != 32 or len(esk) != 32):
        raise ValueError("invalid outgoing plaintext component size")
    return (bytes([OUTGOING_PLAINTEXT_VERSION]) + pk_d_enc + pk_d_spend +
            nfk_commitment + esk)


def encrypt_outgoing(ovk: bytes, public: PublicOutput, recipient_note: bytes,
                     plaintext: bytes) -> tuple[bytes, dict[str, bytes]]:
    if len(plaintext) != OUTGOING_PLAINTEXT_BYTES:
        raise ValueError("invalid outgoing plaintext length")
    epk = recipient_note[:32]
    key, salt_preimage, salt, recipient_hash = recovery_key(
        ovk, public, recipient_note)
    aad = outgoing_aad(public, recipient_note)
    ciphertext, tag = base.aead_encrypt(key, b"\0" * 12, aad, plaintext)
    envelope = bytes([ENVELOPE_VERSION]) + recipient_note + ciphertext + tag
    if len(envelope) != V3_ENVELOPE_BYTES:
        raise AssertionError("v3 envelope length drift")
    return envelope, {
        "public_context": context_bytes(public, epk),
        "salt_preimage": salt_preimage,
        "salt": salt,
        "recipient_ciphertext_hash": recipient_hash,
        "recovery_key": key,
        "aad": aad,
        "ciphertext": ciphertext,
        "tag": tag,
    }


def valid_xonly(raw: bytes) -> bool:
    try:
        base.lift_even_x(raw)
        return True
    except ValueError:
        return False


def recover(ovk: bytes, public: PublicOutput, encoded: bytes, *, height: int,
            spend_auth_activation_height: int,
            outgoing_activation_height: int) -> RecoveryResult:
    validate_activation_policy(spend_auth_activation_height, outgoing_activation_height)
    if len(encoded) == LEGACY_ENCRYPTED_NOTE_BYTES:
        return RecoveryResult(Verdict.LEGACY_NO_OUTGOING_RECOVERY)
    if len(encoded) != V3_ENVELOPE_BYTES:
        return RecoveryResult(Verdict.INVALID_LENGTH)
    if encoded[0] != ENVELOPE_VERSION:
        return RecoveryResult(Verdict.UNSUPPORTED_ENVELOPE_VERSION)
    if not active(height, spend_auth_activation_height):
        return RecoveryResult(Verdict.SPEND_AUTHORITY_REQUIRED)
    if not active(height, outgoing_activation_height):
        return RecoveryResult(Verdict.OUTGOING_RECOVERY_INACTIVE)

    recipient_note = encoded[1:1 + LEGACY_ENCRYPTED_NOTE_BYTES]
    epk = recipient_note[:32]
    outgoing = encoded[1 + LEGACY_ENCRYPTED_NOTE_BYTES:]
    key, _, _, _ = recovery_key(ovk, public, recipient_note)
    try:
        plaintext = base.aead_decrypt(
            key, b"\0" * 12, outgoing_aad(public, recipient_note),
            outgoing[:-16], outgoing[-16:])
    except ValueError:
        # AEAD intentionally cannot reveal whether the key was wrong or bytes
        # were tampered.  Per-output scan misses are not operator errors.
        return RecoveryResult(Verdict.NOT_FOR_VIEWER_OR_TAMPERED)

    if plaintext[0] != OUTGOING_PLAINTEXT_VERSION:
        return RecoveryResult(Verdict.UNSUPPORTED_PLAINTEXT_VERSION)
    pk_d_enc = plaintext[1:33]
    pk_d_spend = plaintext[33:65]
    nfk_commitment = plaintext[65:97]
    esk_bytes = plaintext[97:129]
    if not valid_xonly(pk_d_enc):
        return RecoveryResult(Verdict.INVALID_ENCRYPTION_KEY)
    if not valid_xonly(pk_d_spend):
        return RecoveryResult(Verdict.INVALID_SPEND_KEY)
    nfk_commitment_value = int.from_bytes(nfk_commitment, "big")
    if nfk_commitment_value == 0 or nfk_commitment_value >= base.N:
        return RecoveryResult(Verdict.INVALID_NULLIFIER_KEY_COMMITMENT)
    esk = int.from_bytes(esk_bytes, "big")
    if esk == 0 or esk >= base.N:
        return RecoveryResult(Verdict.INVALID_EPHEMERAL_SECRET)

    recipient = base.lift_even_x(pk_d_enc)
    shared = base.even(base.point_mul(esk, recipient))
    # d is encrypted, so recipient decryption precedes the epk relation check.
    note_key = base.hkdf(epk, base.b32(shared[0]), b"DIN/v7/shielded/note")
    try:
        note_plaintext = base.aead_decrypt(
            note_key, b"\0" * 12, epk, recipient_note[32:-16], recipient_note[-16:])
    except ValueError:
        return RecoveryResult(Verdict.RECIPIENT_AUTHENTICATION_FAILED)

    d = note_plaintext[:11]
    value = int.from_bytes(note_plaintext[11:19], "little")
    rcm = note_plaintext[19:51]
    memo = note_plaintext[51:563]
    pd = base.hash_to_point(d)
    expected_epk = base.point_mul(esk, pd)
    assert expected_epk is not None
    if expected_epk[1] & 1 or base.b32(expected_epk[0]) != epk:
        return RecoveryResult(Verdict.EPHEMERAL_KEY_MISMATCH)
    expected_commitment = auth_note_commitment(
        d, pk_d_spend, nfk_commitment, value, rcm)
    if expected_commitment != public.commitment:
        return RecoveryResult(Verdict.COMMITMENT_MISMATCH)

    return RecoveryResult(Verdict.OK, {
        "d": d,
        "value": value,
        "rcm": rcm,
        "memo": memo,
        "pk_d_enc": pk_d_enc,
        "pk_d_spend": pk_d_spend,
        "nfk_commitment": nfk_commitment,
        "esk_normalized": esk_bytes,
        "recipient_address_payload": d + pk_d_enc + pk_d_spend + nfk_commitment,
        "recomputed_commitment": expected_commitment,
    })


def reencrypt_with_plaintext(ovk: bytes, public: PublicOutput, envelope: bytes,
                             plaintext: bytes) -> bytes:
    recipient_note = envelope[1:1 + LEGACY_ENCRYPTED_NOTE_BYTES]
    rebuilt, _ = encrypt_outgoing(ovk, public, recipient_note, plaintext)
    return rebuilt


def rebind_public_context(ovk: bytes, old_public: PublicOutput, new_public: PublicOutput,
                          envelope: bytes) -> bytes:
    recipient_note = envelope[1:1 + LEGACY_ENCRYPTED_NOTE_BYTES]
    outgoing = envelope[1 + LEGACY_ENCRYPTED_NOTE_BYTES:]
    old_key, _, _, _ = recovery_key(ovk, old_public, recipient_note)
    plaintext = base.aead_decrypt(
        old_key, b"\0" * 12, outgoing_aad(old_public, recipient_note),
        outgoing[:-16], outgoing[-16:])
    rebuilt, _ = encrypt_outgoing(ovk, new_public, recipient_note, plaintext)
    return rebuilt


def mutate_at(raw: bytes, offset: int) -> bytes:
    changed = bytearray(raw)
    changed[offset] ^= 1
    return bytes(changed)


def build_vectors() -> dict[str, Any]:
    upstream = base.build_vectors()
    derivation = upstream["derivation"]
    note = upstream["note"]
    address = derivation["address_j0"]
    ovk = bytes.fromhex(derivation["ovk"])
    recipient_note = bytes.fromhex(note["encrypted_note"])
    d = bytes.fromhex(address["d"])
    pk_d_enc = bytes.fromhex(address["pk_d_enc"])
    pk_d_spend = bytes.fromhex(address["pk_d_spend"])
    nfk_commitment = bytes.fromhex(address["nullifier_key_commitment"])
    recipient_spend_scalar = bytes.fromhex(address["spend_scalar"])
    esk = bytes.fromhex(note["esk_normalized"])
    rcm = bytes.fromhex(note["rcm"])
    memo = bytes.fromhex(note["memo"])
    legacy_spend_key = bytes.fromhex(upstream["commitment"]["sk_note"])
    legacy_commitment = bytes.fromhex(upstream["commitment"]["note_commitment"])
    value = 100_000_000

    commitment = auth_note_commitment(d, pk_d_spend, nfk_commitment, value, rcm)
    blind = base.valid_scalar(base.h256(b"DIN outgoing-view vector rcv"))
    cv = value_commitment(blind, value)
    public = PublicOutput(commitment, cv)
    plaintext = outgoing_plaintext(pk_d_enc, pk_d_spend, nfk_commitment, esk)
    envelope, internals = encrypt_outgoing(ovk, public, recipient_note, plaintext)

    spend_height = 100
    outgoing_height = 100
    result = recover(
        ovk, public, envelope, height=100,
        spend_auth_activation_height=spend_height,
        outgoing_activation_height=outgoing_height)
    if result.verdict != Verdict.OK or result.recovered is None:
        raise AssertionError(f"canonical recovery failed: {result.verdict}")

    wrong_ovk = mutate_at(ovk, 0)
    invalid_enc_plaintext = plaintext[:1] + b"\xff" * 32 + plaintext[33:]
    invalid_spend_plaintext = plaintext[:33] + b"\xff" * 32 + plaintext[65:]
    zero_nfk_commitment_plaintext = plaintext[:65] + b"\0" * 32 + plaintext[97:]
    order_nfk_commitment_plaintext = plaintext[:65] + base.b32(base.N) + plaintext[97:]
    zero_esk_plaintext = plaintext[:97] + b"\0" * 32
    order_esk_plaintext = plaintext[:97] + base.b32(base.N)
    mismatched_esk = (int.from_bytes(esk, "big") + 1) % base.N
    if mismatched_esk == 0:
        mismatched_esk = 1
    mismatched_esk_plaintext = plaintext[:97] + base.b32(mismatched_esk)

    # A bare esk mutation fails recipient authentication before the epk
    # relation is checked.  Construct a stronger mutant whose embedded note is
    # validly encrypted under the mutated esk while retaining the original epk;
    # this reaches and proves the dedicated EphemeralKeyMismatch branch.
    original_note_plaintext = bytes.fromhex(note["plaintext"])
    mismatched_shared = base.even(base.point_mul(mismatched_esk, base.lift_even_x(pk_d_enc)))
    mismatched_note_key = base.hkdf(
        recipient_note[:32], base.b32(mismatched_shared[0]), b"DIN/v7/shielded/note")
    mismatched_note_ct, mismatched_note_tag = base.aead_encrypt(
        mismatched_note_key, b"\0" * 12, recipient_note[:32], original_note_plaintext)
    mismatched_recipient_note = recipient_note[:32] + mismatched_note_ct + mismatched_note_tag
    mismatched_epk_envelope, _ = encrypt_outgoing(
        ovk, public, mismatched_recipient_note, mismatched_esk_plaintext)
    noncanonical_esk_plaintext = plaintext[:97] + base.b32(
        base.N - int.from_bytes(esk, "big"))
    noncanonical_esk_envelope = reencrypt_with_plaintext(
        ovk, public, envelope, noncanonical_esk_plaintext)

    recipient_tampered = mutate_at(recipient_note, 100)
    recipient_tampered_envelope, _ = encrypt_outgoing(
        ovk, public, recipient_tampered, plaintext)
    wrong_commitment = mutate_at(commitment, 0)
    wrong_public = PublicOutput(wrong_commitment, cv)
    commitment_mismatch_envelope = rebind_public_context(ovk, public, wrong_public, envelope)
    wrong_cv = mutate_at(cv, 1)
    transplanted_recipient = mutate_at(recipient_note, 100)
    transplanted_envelope = (
        bytes([ENVELOPE_VERSION]) + transplanted_recipient +
        envelope[1 + LEGACY_ENCRYPTED_NOTE_BYTES:])

    failures = {
        "legacy_v1": Verdict.LEGACY_NO_OUTGOING_RECOVERY,
        "truncated_v3": Verdict.INVALID_LENGTH,
        "trailing_v3": Verdict.INVALID_LENGTH,
        "unknown_envelope_version": Verdict.UNSUPPORTED_ENVELOPE_VERSION,
        "before_spend_authority": Verdict.SPEND_AUTHORITY_REQUIRED,
        "before_outgoing_activation": Verdict.OUTGOING_RECOVERY_INACTIVE,
        "wrong_ovk": Verdict.NOT_FOR_VIEWER_OR_TAMPERED,
        "tampered_outgoing_tag": Verdict.NOT_FOR_VIEWER_OR_TAMPERED,
        "unknown_plaintext_version": Verdict.UNSUPPORTED_PLAINTEXT_VERSION,
        "invalid_encryption_key": Verdict.INVALID_ENCRYPTION_KEY,
        "invalid_spend_key": Verdict.INVALID_SPEND_KEY,
        "invalid_nullifier_key_commitment": Verdict.INVALID_NULLIFIER_KEY_COMMITMENT,
        "noncanonical_nullifier_key_commitment": Verdict.INVALID_NULLIFIER_KEY_COMMITMENT,
        "zero_ephemeral_secret": Verdict.INVALID_EPHEMERAL_SECRET,
        "curve_order_ephemeral_secret": Verdict.INVALID_EPHEMERAL_SECRET,
        "ephemeral_key_mismatch": Verdict.EPHEMERAL_KEY_MISMATCH,
        "noncanonical_ephemeral_secret": Verdict.EPHEMERAL_KEY_MISMATCH,
        "recipient_authentication_failure": Verdict.RECIPIENT_AUTHENTICATION_FAILED,
        "recipient_ciphertext_transplant": Verdict.NOT_FOR_VIEWER_OR_TAMPERED,
        "wrong_commitment_context": Verdict.NOT_FOR_VIEWER_OR_TAMPERED,
        "wrong_value_commitment_context": Verdict.NOT_FOR_VIEWER_OR_TAMPERED,
        "commitment_mismatch": Verdict.COMMITMENT_MISMATCH,
    }

    def h(raw: bytes) -> str:
        return raw.hex()

    return {
        "format": "dinero-shielded-outgoing-view-v3-independent-vectors",
        "version": 1,
        "status": "prospective-design-only-not-production",
        "oracle": (
            "Python 3 standard library plus the independent shielded-protocol primitive "
            "oracle; no Dinero C++ or linked crypto library"),
        "activation": {
            "vector_height": 100,
            "spend_authority_activation_height": spend_height,
            "outgoing_recovery_activation_height": outgoing_height,
            "dormant_sentinel": U32_MAX,
            "rule": (
                "outgoing recovery activation must be non-dormant and at or "
                "after spend-authority activation"),
        },
        "sizes": {
            "legacy_recipient_ciphertext": LEGACY_ENCRYPTED_NOTE_BYTES,
            "envelope_version": ENVELOPE_VERSION,
            "outgoing_plaintext": OUTGOING_PLAINTEXT_BYTES,
            "outgoing_ciphertext_and_tag": OUTGOING_CIPHERTEXT_BYTES,
            "v3_envelope": V3_ENVELOPE_BYTES,
            "v3_envelope_compact_size": base.compact_size(V3_ENVELOPE_BYTES).hex(),
        },
        "domains": {
            "salt": SALT_DOMAIN.decode(),
            "key": KEY_DOMAIN.decode(),
            "aad": AAD_DOMAIN.decode(),
        },
        "source": {
            "ovk": h(ovk), "d": h(d), "pk_d_enc": h(pk_d_enc),
            "pk_d_spend": h(pk_d_spend),
            "nfk_commitment": h(nfk_commitment), "esk_normalized": h(esk),
            "value": value, "rcm": h(rcm), "memo": h(memo),
            "recipient_encrypted_note_v1": h(recipient_note),
            "rcv": f"{blind:064x}", "value_commitment": h(cv),
            "authenticated_note_commitment": h(commitment),
        },
        "security_boundary": {
            "recipient_spend_scalar": h(recipient_spend_scalar),
            "legacy_rcm_derived_spend_key": h(legacy_spend_key),
            "legacy_note_commitment": h(legacy_commitment),
            "authenticated_note_commitment": h(commitment),
            "commitments_differ": legacy_commitment != commitment,
            "outgoing_plaintext_contains_recipient_spend_scalar": (
                recipient_spend_scalar in plaintext),
            "outgoing_plaintext_contains_legacy_spend_key": legacy_spend_key in plaintext,
            "outgoing_plaintext_contains_rcm": rcm in plaintext,
        },
        "derivation": {
            key: h(value) for key, value in internals.items()
            if key in ("public_context", "recipient_ciphertext_hash", "salt_preimage",
                       "salt", "recovery_key", "aad")
        },
        "outgoing": {
            "plaintext": h(plaintext),
            "ciphertext": h(internals["ciphertext"]),
            "tag": h(internals["tag"]),
            "envelope_v3": h(envelope),
        },
        "recovery": {
            "verdict": result.verdict,
            "d": h(result.recovered["d"]),
            "value": result.recovered["value"],
            "rcm": h(result.recovered["rcm"]),
            "memo": h(result.recovered["memo"]),
            "pk_d_enc": h(result.recovered["pk_d_enc"]),
            "pk_d_spend": h(result.recovered["pk_d_spend"]),
            "nfk_commitment": h(result.recovered["nfk_commitment"]),
            "esk_normalized": h(result.recovered["esk_normalized"]),
            "recipient_address_payload": h(result.recovered["recipient_address_payload"]),
            "recomputed_commitment": h(result.recovered["recomputed_commitment"]),
        },
        "failure_classes": failures,
        "compatibility": {
            "legacy_v1": "accepted for incoming viewing; outgoing history is unrecoverable",
            "v3_before_spend_authority": "must not be emitted and is rejected by upgraded wallets",
            "v3_after_both_activations": (
                "incoming viewing and outgoing recovery are available to upgraded wallets"),
            "old_wallet_receiving_v3": (
                "ignored as an unknown encrypted-note length; never reinterpreted as legacy"),
            "consensus": (
                "encrypted_note remains opaque; this package changes no consensus parser "
                "or activation"),
        },
        "mutation_fixtures": {
            "truncated_v3": h(envelope[:-1]),
            "trailing_v3": h(envelope + b"\0"),
            "unknown_envelope_version": h(bytes([4]) + envelope[1:]),
            "tampered_outgoing_tag": h(envelope[:-1] + bytes([envelope[-1] ^ 1])),
            "unknown_plaintext_version": h(reencrypt_with_plaintext(
                ovk, public, envelope, b"\x03" + plaintext[1:])),
            "invalid_encryption_key": h(reencrypt_with_plaintext(
                ovk, public, envelope, invalid_enc_plaintext)),
            "invalid_spend_key": h(reencrypt_with_plaintext(
                ovk, public, envelope, invalid_spend_plaintext)),
            "invalid_nullifier_key_commitment": h(reencrypt_with_plaintext(
                ovk, public, envelope, zero_nfk_commitment_plaintext)),
            "noncanonical_nullifier_key_commitment": h(reencrypt_with_plaintext(
                ovk, public, envelope, order_nfk_commitment_plaintext)),
            "zero_ephemeral_secret": h(reencrypt_with_plaintext(
                ovk, public, envelope, zero_esk_plaintext)),
            "curve_order_ephemeral_secret": h(reencrypt_with_plaintext(
                ovk, public, envelope, order_esk_plaintext)),
            "ephemeral_key_mismatch": h(mismatched_epk_envelope),
            "noncanonical_ephemeral_secret": h(noncanonical_esk_envelope),
            "recipient_authentication_failure": h(recipient_tampered_envelope),
            "recipient_ciphertext_transplant": h(transplanted_envelope),
            "commitment_mismatch_public": h(wrong_commitment),
            "wrong_value_commitment_public": h(wrong_cv),
            "commitment_mismatch_envelope": h(commitment_mismatch_envelope),
        },
    }


def exercise_failure_classes(vectors: dict[str, Any]) -> dict[str, str]:
    src = vectors["source"]
    mutations = vectors["mutation_fixtures"]
    activation = vectors["activation"]
    ovk = bytes.fromhex(src["ovk"])
    public = PublicOutput(bytes.fromhex(src["authenticated_note_commitment"]),
                          bytes.fromhex(src["value_commitment"]))
    envelope = bytes.fromhex(vectors["outgoing"]["envelope_v3"])
    spend = activation["spend_authority_activation_height"]
    outgoing = activation["outgoing_recovery_activation_height"]

    def run(encoded: bytes, *, key: bytes = ovk, height: int = 100,
            pub: PublicOutput = public, spend_height: int = spend,
            outgoing_height: int = outgoing) -> str:
        return recover(key, pub, encoded, height=height,
                       spend_auth_activation_height=spend_height,
                       outgoing_activation_height=outgoing_height).verdict

    wrong_public = PublicOutput(bytes.fromhex(mutations["commitment_mismatch_public"]), public.cv)
    wrong_cv_public = PublicOutput(
        public.commitment, bytes.fromhex(mutations["wrong_value_commitment_public"]))
    return {
        "legacy_v1": run(bytes.fromhex(src["recipient_encrypted_note_v1"])),
        "truncated_v3": run(bytes.fromhex(mutations["truncated_v3"])),
        "trailing_v3": run(bytes.fromhex(mutations["trailing_v3"])),
        "unknown_envelope_version": run(bytes.fromhex(mutations["unknown_envelope_version"])),
        "before_spend_authority": run(envelope, height=99),
        "before_outgoing_activation": run(envelope, height=100, outgoing_height=101),
        "wrong_ovk": run(envelope, key=mutate_at(ovk, 0)),
        "tampered_outgoing_tag": run(bytes.fromhex(mutations["tampered_outgoing_tag"])),
        "unknown_plaintext_version": run(bytes.fromhex(mutations["unknown_plaintext_version"])),
        "invalid_encryption_key": run(bytes.fromhex(mutations["invalid_encryption_key"])),
        "invalid_spend_key": run(bytes.fromhex(mutations["invalid_spend_key"])),
        "invalid_nullifier_key_commitment": run(
            bytes.fromhex(mutations["invalid_nullifier_key_commitment"])),
        "noncanonical_nullifier_key_commitment": run(
            bytes.fromhex(mutations["noncanonical_nullifier_key_commitment"])),
        "zero_ephemeral_secret": run(bytes.fromhex(mutations["zero_ephemeral_secret"])),
        "curve_order_ephemeral_secret": run(
            bytes.fromhex(mutations["curve_order_ephemeral_secret"])),
        "ephemeral_key_mismatch": run(bytes.fromhex(mutations["ephemeral_key_mismatch"])),
        "noncanonical_ephemeral_secret": run(
            bytes.fromhex(mutations["noncanonical_ephemeral_secret"])),
        "recipient_authentication_failure": run(
            bytes.fromhex(mutations["recipient_authentication_failure"])),
        "recipient_ciphertext_transplant": run(
            bytes.fromhex(mutations["recipient_ciphertext_transplant"])),
        "wrong_commitment_context": run(envelope, pub=wrong_public),
        "wrong_value_commitment_context": run(envelope, pub=wrong_cv_public),
        "commitment_mismatch": run(
            bytes.fromhex(mutations["commitment_mismatch_envelope"]), pub=wrong_public),
    }


def self_test(vectors: dict[str, Any]) -> None:
    observed = exercise_failure_classes(vectors)
    expected = vectors["failure_classes"]
    if observed != expected:
        raise AssertionError(f"failure-class mismatch\nexpected={expected}\nobserved={observed}")

    # The exact sentinel edge must stay dormant even when the untrusted height
    # itself equals UINT32_MAX.  A bare `height >= activation` fails this test.
    if active(U32_MAX, U32_MAX):
        raise AssertionError("UINT32_MAX dormant sentinel activated at its degenerate height")
    for spend, outgoing in ((U32_MAX, 100), (101, 100)):
        try:
            validate_activation_policy(spend, outgoing)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe outgoing/spend-authority activation ordering accepted")

    # Prove every named mutation changes bytes and every failure class is
    # exercised.  Equality here would make the mutation control vacuous.
    envelope = vectors["outgoing"]["envelope_v3"]
    mutations = vectors["mutation_fixtures"]
    byte_mutants = [value for key, value in mutations.items()
                    if key.endswith("v3") or key.endswith("version") or
                    key.endswith("tag") or key.endswith("key") or
                    key.endswith("secret") or key.endswith("mismatch") or
                    key.endswith("failure") or key.endswith("envelope")]
    if any(value == envelope for value in byte_mutants):
        raise AssertionError("mutation fixture did not alter the canonical envelope")
    declared_verdicts = {
        value for name, value in vars(Verdict).items()
        if name.isupper() and isinstance(value, str)
    }
    exercised_verdicts = set(observed.values())
    exercised_verdicts.add(vectors["recovery"]["verdict"])
    if exercised_verdicts != declared_verdicts:
        missing = sorted(declared_verdicts - exercised_verdicts)
        unexpected = sorted(exercised_verdicts - declared_verdicts)
        raise AssertionError(
            "declared verdict coverage mismatch: "
            f"missing={missing}, unexpected={unexpected}")

    # Explicitly pin the security boundary: the outgoing plaintext does not
    # contain rcm, even though authorized recovery later obtains rcm by
    # decrypting the embedded recipient ciphertext.
    plaintext = bytes.fromhex(vectors["outgoing"]["plaintext"])
    rcm = bytes.fromhex(vectors["source"]["rcm"])
    if rcm in plaintext:
        raise AssertionError("outgoing plaintext directly exposes rcm")
    security = vectors["security_boundary"]
    if not security["commitments_differ"]:
        raise AssertionError("legacy and recipient-bound commitments unexpectedly coincide")
    for field in (
            "outgoing_plaintext_contains_recipient_spend_scalar",
            "outgoing_plaintext_contains_legacy_spend_key",
            "outgoing_plaintext_contains_rcm"):
        if security[field]:
            raise AssertionError(f"outgoing plaintext crossed spending boundary: {field}")

    recipient = bytes.fromhex(vectors["source"]["recipient_encrypted_note_v1"])
    raw_envelope = bytes.fromhex(envelope)
    if len(recipient) != LEGACY_ENCRYPTED_NOTE_BYTES:
        raise AssertionError("legacy recipient ciphertext length drift")
    if len(plaintext) != OUTGOING_PLAINTEXT_BYTES:
        raise AssertionError("outgoing plaintext length drift")
    if len(raw_envelope) != V3_ENVELOPE_BYTES or raw_envelope[0] != ENVELOPE_VERSION:
        raise AssertionError("v3 envelope version or length drift")
    if raw_envelope[1:1 + LEGACY_ENCRYPTED_NOTE_BYTES] != recipient:
        raise AssertionError("v3 did not embed the legacy recipient ciphertext verbatim")
    if vectors["sizes"]["v3_envelope_compact_size"] != "fdf502":
        raise AssertionError("757-byte CompactSize encoding drift")

    source = vectors["source"]
    public = PublicOutput(
        bytes.fromhex(source["authenticated_note_commitment"]),
        bytes.fromhex(source["value_commitment"]))
    ovk = bytes.fromhex(source["ovk"])
    original_key, _, _, _ = recovery_key(ovk, public, recipient)
    changed_contexts = {
        "commitment": (
            PublicOutput(mutate_at(public.commitment, 0), public.cv), recipient),
        "value commitment": (
            PublicOutput(public.commitment, mutate_at(public.cv, 1)), recipient),
        "ephemeral key": (public, mutate_at(recipient, 0)),
        "recipient ciphertext": (public, mutate_at(recipient, 100)),
    }
    for name, (changed_public, changed_recipient) in changed_contexts.items():
        changed_key, _, _, _ = recovery_key(
            ovk, changed_public, changed_recipient)
        if original_key == changed_key:
            raise AssertionError(f"{name} is not load-bearing in key derivation")


def canonical_json(vectors: dict[str, Any]) -> str:
    return json.dumps(vectors, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output", type=pathlib.Path,
        default=pathlib.Path("tests/vectors/shielded_outgoing_view_v3_external.json"))
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    vectors = build_vectors()
    if args.self_test:
        self_test(vectors)
    rendered = canonical_json(vectors)
    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"missing vector file: {args.output}", file=sys.stderr)
            return 1
        if existing != rendered:
            print(f"outgoing-view vectors differ: regenerate {args.output}", file=sys.stderr)
            return 1
        print(f"PASS: independent outgoing-view vectors match {args.output}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
