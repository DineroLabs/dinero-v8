#!/usr/bin/env python3
"""Independent Dinero shielded-v1 vector oracle.

This file intentionally uses only Python's standard library and contains no
Dinero source imports.  It is a second implementation of the byte-level rules
in docs/specs/shielded_protocol_v1.md.  Its checked-in JSON output is consumed
by the C++ conformance test; --check refuses drift and --self-test proves that
representative mutations are detected.

It does not implement a second Spartan or Borromean prover.  For those systems
it fixes the consensus transcript framing and proof/container encodings.  That
boundary is explicit in the generated metadata and companion documentation.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import pathlib
import struct
import sys
from typing import Iterable, Optional


P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (
    0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
    0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8,
)
BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def b32(x: int) -> bytes:
    return x.to_bytes(32, "big")


def h256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def valid_scalar(raw: bytes) -> int:
    value = int.from_bytes(raw, "big")
    while value == 0 or value >= N:
        raw = h256(raw)
        value = int.from_bytes(raw, "big")
    return value


def inv(x: int, modulus: int = P) -> int:
    return pow(x, modulus - 2, modulus)


Point = Optional[tuple[int, int]]


def point_add(a: Point, b: Point) -> Point:
    if a is None:
        return b
    if b is None:
        return a
    ax, ay = a
    bx, by = b
    if ax == bx and (ay + by) % P == 0:
        return None
    if a == b:
        slope = (3 * ax * ax) * inv(2 * ay) % P
    else:
        slope = (by - ay) * inv((bx - ax) % P) % P
    x = (slope * slope - ax - bx) % P
    return x, (slope * (ax - x) - ay) % P


def point_mul(k: int, point: Point = G) -> Point:
    result = None
    addend = point
    while k:
        if k & 1:
            result = point_add(result, addend)
        addend = point_add(addend, addend)
        k >>= 1
    return result


def compressed_point(k: int) -> bytes:
    point = point_mul(k)
    if point is None:
        raise ValueError("point at infinity has no compressed encoding")
    x, y = point
    return bytes([0x02 | (y & 1)]) + b32(x)


def even(point: Point) -> tuple[int, int]:
    assert point is not None
    x, y = point
    return (x, y if y % 2 == 0 else P - y)


def normalize_scalar(k: int) -> tuple[int, tuple[int, int]]:
    point = point_mul(k)
    assert point is not None
    if point[1] & 1:
        k = N - k
        point = (point[0], P - point[1])
    return k, point


def lift_even_x(raw: bytes) -> tuple[int, int]:
    x = int.from_bytes(raw, "big")
    if x >= P:
        raise ValueError("x is not canonical")
    y = pow((pow(x, 3, P) + 7) % P, (P + 1) // 4, P)
    if (y * y - x * x * x - 7) % P:
        raise ValueError("x is not on secp256k1")
    return even((x, y))


def svdw(t: int) -> tuple[int, int]:
    """ElementsProject secp256k1-zkp generator module's SvdW map."""
    neg_c = 0xF5D2D456CAF80E20DCC88F3D586869D339E092EA25EB132B8272D850E32A03DD
    d = 0x851695D49A83F8EF919BB86153CBCB16630FB68AED0A766A3EC693D68E6AFA40
    t2 = t * t % P
    wd = (t2 + 8) % P
    x3d = (-3 * t2) % P
    joint_inv = inv(wd * x3d % P) if t else 0
    x1 = (d + neg_c * t2 % P * x3d % P * joint_inv) % P
    x2 = -(x1 + 1) % P
    x3 = (1 + pow(wd, 3, P) * joint_inv) % P
    choices = []
    for x in (x1, x2, x3):
        rhs = (pow(x, 3, P) + 7) % P
        y = pow(rhs, (P + 1) // 4, P)
        choices.append((x, y, y * y % P == rhs))
    point = next((x, y) for x, y, square in choices if square)
    if t & 1:
        point = (point[0], P - point[1])
    return point


def generator_generate(seed: bytes) -> tuple[int, int]:
    first = int.from_bytes(h256(b"1st generation: " + seed), "big")
    second = int.from_bytes(h256(b"2nd generation: " + seed), "big")
    if first >= P or second >= P:
        raise ValueError("generator seed hash outside field")
    result = point_add(svdw(first), svdw(second))
    assert result is not None
    return result


def poseidon_constants() -> list[list[int]]:
    out = []
    tag = b"PoseidonC_secp256k1"
    for round_no in range(64):
        row = []
        for element in range(3):
            raw = h256(tag + struct.pack(">I", round_no) + struct.pack(">I", element))
            row.append(valid_scalar(raw))
        out.append(row)
    return out


POSEIDON_CONSTANTS = poseidon_constants()


def poseidon_trace(a: int, b: int) -> tuple[int, list[list[int]]]:
    state = [0, a % N, b % N]
    trace = [state.copy()]
    for round_no, constants in enumerate(POSEIDON_CONSTANTS):
        state = [(state[i] + constants[i]) % N for i in range(3)]
        if round_no < 4 or round_no >= 60:
            state = [pow(x, 5, N) for x in state]
        else:
            state[0] = pow(state[0], 5, N)
        total = sum(state) % N
        state = [(total + x) % N for x in state]
        trace.append(state.copy())
    return state[1], trace


def poseidon(a: bytes, b: bytes) -> bytes:
    result, _ = poseidon_trace(int.from_bytes(a, "big"), int.from_bytes(b, "big"))
    return b32(result)


def dst(text: str) -> bytes:
    raw = text.encode("ascii")
    if len(raw) > 32:
        raise ValueError("DST too long")
    return raw.ljust(32, b"\0")


def canonical_seed() -> bytes:
    tag = b"DIN/v7/shielded/derivation/v1"
    first = tag.ljust(32, b"\0")
    return first + bytes(x ^ 0xFF for x in first)


def bip32_account(seed: bytes, account: int) -> bytes:
    digest = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    key = int.from_bytes(digest[:32], "big")
    chain = digest[32:]
    for index in (99, 1448, account):
        child = index | 0x80000000
        digest = hmac.new(chain, b"\0" + b32(key) + struct.pack(">I", child), hashlib.sha512).digest()
        tweak = int.from_bytes(digest[:32], "big")
        if tweak >= N or (key + tweak) % N == 0:
            raise ValueError("invalid BIP32 child")
        key = (key + tweak) % N
        chain = digest[32:]
    return b32(key)


def derive_account(seed: bytes, account: int) -> dict[str, bytes]:
    sk = bip32_account(seed, account)
    ask_raw = poseidon(sk, dst("DIN/v7/shielded/ask"))
    nsk_raw = poseidon(sk, dst("DIN/v7/shielded/nsk"))
    ask, ak = normalize_scalar(int.from_bytes(ask_raw, "big"))
    nsk, nk = normalize_scalar(int.from_bytes(nsk_raw, "big"))
    ak_x, nk_x = b32(ak[0]), b32(nk[0])
    return {
        "sk": sk,
        "ask": b32(ask),
        "nsk": b32(nsk),
        "ovk": poseidon(sk, dst("DIN/v7/shielded/ovk")),
        "dk": poseidon(sk, dst("DIN/v7/shielded/dk")),
        "ak": ak_x,
        "nk": nk_x,
        "ivk": poseidon(ak_x, nk_x),
    }


def rotl32(x: int, n: int) -> int:
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def chacha_block(key: bytes, iv16: bytes) -> bytes:
    words = list(struct.unpack("<4I", b"expand 32-byte k"))
    words += list(struct.unpack("<8I", key))
    words += list(struct.unpack("<4I", iv16))
    work = words.copy()

    def qr(a: int, b: int, c: int, d: int) -> None:
        work[a] = (work[a] + work[b]) & 0xFFFFFFFF; work[d] ^= work[a]; work[d] = rotl32(work[d], 16)
        work[c] = (work[c] + work[d]) & 0xFFFFFFFF; work[b] ^= work[c]; work[b] = rotl32(work[b], 12)
        work[a] = (work[a] + work[b]) & 0xFFFFFFFF; work[d] ^= work[a]; work[d] = rotl32(work[d], 8)
        work[c] = (work[c] + work[d]) & 0xFFFFFFFF; work[b] ^= work[c]; work[b] = rotl32(work[b], 7)

    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<16I", *((work[i] + words[i]) & 0xFFFFFFFF for i in range(16)))


def chacha_xor(key: bytes, counter: int, nonce: bytes, data: bytes) -> bytes:
    out = bytearray()
    for offset in range(0, len(data), 64):
        stream = chacha_block(key, struct.pack("<I", counter) + nonce)
        block = data[offset:offset + 64]
        out.extend(a ^ b for a, b in zip(block, stream))
        counter = (counter + 1) & 0xFFFFFFFF
    return bytes(out)


def diversifier(dk: bytes, j: int) -> bytes:
    return chacha_xor(dk, 0, struct.pack("<Q", j) + b"\0" * 4, b"\0" * 11)


def poly1305(msg: bytes, one_time_key: bytes) -> bytes:
    r = int.from_bytes(one_time_key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(one_time_key[16:], "little")
    acc = 0
    mod = (1 << 130) - 5
    for offset in range(0, len(msg), 16):
        chunk = msg[offset:offset + 16]
        acc = (acc + int.from_bytes(chunk + b"\x01", "little")) * r % mod
    return ((acc + s) % (1 << 128)).to_bytes(16, "little")


def pad16(data: bytes) -> bytes:
    return b"" if len(data) % 16 == 0 else b"\0" * (16 - len(data) % 16)


def aead_encrypt(key: bytes, nonce: bytes, aad: bytes, plaintext: bytes) -> tuple[bytes, bytes]:
    poly_key = chacha_block(key, struct.pack("<I", 0) + nonce)[:32]
    ciphertext = chacha_xor(key, 1, nonce, plaintext)
    mac_data = aad + pad16(aad) + ciphertext + pad16(ciphertext)
    mac_data += struct.pack("<Q", len(aad)) + struct.pack("<Q", len(ciphertext))
    return ciphertext, poly1305(mac_data, poly_key)


def aead_decrypt(key: bytes, nonce: bytes, aad: bytes,
                 ciphertext: bytes, tag: bytes) -> bytes:
    poly_key = chacha_block(key, struct.pack("<I", 0) + nonce)[:32]
    mac_data = aad + pad16(aad) + ciphertext + pad16(ciphertext)
    mac_data += struct.pack("<Q", len(aad)) + struct.pack("<Q", len(ciphertext))
    expected = poly1305(mac_data, poly_key)
    if not hmac.compare_digest(expected, tag):
        raise ValueError("note authentication failed")
    return chacha_xor(key, 1, nonce, ciphertext)


def hkdf(salt: bytes, ikm: bytes, info: bytes) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()


def polymod(values: Iterable[int]) -> int:
    generators = (0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3)
    chk = 1
    for value in values:
        top = chk >> 25
        chk = ((chk & 0x1FFFFFF) << 5) ^ value
        for i, generator in enumerate(generators):
            if (top >> i) & 1:
                chk ^= generator
    return chk


def hrp_expand(hrp: str) -> list[int]:
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def convertbits(data: bytes, from_bits: int, to_bits: int, pad: bool = True) -> list[int]:
    acc = 0
    bits = 0
    result = []
    maxv = (1 << to_bits) - 1
    for value in data:
        if value >> from_bits:
            raise ValueError("invalid convertbits input")
        acc = (acc << from_bits) | value
        bits += from_bits
        while bits >= to_bits:
            bits -= to_bits
            result.append((acc >> bits) & maxv)
    if pad and bits:
        result.append((acc << (to_bits - bits)) & maxv)
    elif not pad and (bits >= from_bits or ((acc << (to_bits - bits)) & maxv)):
        raise ValueError("non-canonical padding")
    return result


def bech32m(hrp: str, payload: bytes) -> str:
    data = convertbits(payload, 8, 5)
    values = hrp_expand(hrp) + data + [0] * 6
    checksum = polymod(values) ^ 0x2BC830A3
    check = [(checksum >> (5 * (5 - i))) & 31 for i in range(6)]
    return hrp + "1" + "".join(BECH32_CHARSET[x] for x in data + check)


def hash_to_point(d: bytes) -> tuple[int, int]:
    seed = h256(dst("DIN/v7/shielded/div") + d)
    return even(generator_generate(seed))


def derive_address(keys: dict[str, bytes], j: int) -> dict[str, object]:
    d = diversifier(keys["dk"], j)
    pd = hash_to_point(d)
    pk_enc = even(point_mul(int.from_bytes(keys["ivk"], "big"), pd))
    d_padded = d.ljust(32, b"\0")
    spend_raw = int.from_bytes(poseidon(keys["ivk"], d_padded), "big")
    spend_scalar, spend_point = normalize_scalar(spend_raw)
    payload = d + b32(pk_enc[0]) + b32(spend_point[0])
    return {
        "j": j,
        "d": d,
        "p_d": b32(pd[0]),
        "pk_d_enc": b32(pk_enc[0]),
        "spend_scalar": b32(spend_scalar),
        "pk_d_spend": b32(spend_point[0]),
        "payload": payload,
        "mainnet": bech32m("dins", payload),
        "testnet": bech32m("tdins", payload),
        "regtest": bech32m("rdins", payload),
    }


def note_vector(keys: dict[str, bytes], address: dict[str, object]) -> dict[str, bytes]:
    d = address["d"]
    assert isinstance(d, bytes)
    rcm = b"DIN/v7/shielded/note/rcm".ljust(32, b"\0")
    memo = b"DIN test note plain v1".ljust(512, b"\0")
    plaintext = d + struct.pack("<Q", 100_000_000) + rcm + memo
    esk = b"DIN/v7/shielded/note/esk".ljust(32, b"\0")
    pd = lift_even_x(address["p_d"])
    epk_raw = point_mul(int.from_bytes(esk, "big"), pd)
    assert epk_raw is not None
    esk_norm = int.from_bytes(esk, "big")
    if epk_raw[1] & 1:
        esk_norm = N - esk_norm
        epk_raw = (epk_raw[0], P - epk_raw[1])
    recipient = lift_even_x(address["pk_d_enc"])
    shared = even(point_mul(esk_norm, recipient))
    key = hkdf(b32(epk_raw[0]), b32(shared[0]), b"DIN/v7/shielded/note")
    ciphertext, tag = aead_encrypt(key, b"\0" * 12, b32(epk_raw[0]), plaintext)
    recovered = aead_decrypt(key, b"\0" * 12, b32(epk_raw[0]), ciphertext, tag)
    if recovered != plaintext:
        raise AssertionError("independent note decryption did not recover the plaintext")
    encrypted = b32(epk_raw[0]) + ciphertext + tag
    return {
        "rcm": rcm,
        "memo": memo,
        "plaintext": plaintext,
        "esk_input": esk,
        "esk_normalized": b32(esk_norm),
        "epk": b32(epk_raw[0]),
        "shared_secret": b32(shared[0]),
        "aead_key": key,
        "ciphertext": ciphertext,
        "tag": tag,
        "encrypted_note": encrypted,
        "decrypted_plaintext": recovered,
    }


def compact_size(value: int) -> bytes:
    if value < 253:
        return bytes([value])
    if value <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", value)
    if value <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", value)
    return b"\xff" + struct.pack("<Q", value)


def vec(data: bytes) -> bytes:
    return compact_size(len(data)) + data


def range_container(proofs: list[bytes]) -> bytes:
    return compact_size(len(proofs)) + b"".join(vec(proof) for proof in proofs)


def tagged_hash(tag: bytes, message: bytes) -> bytes:
    tag_hash = h256(tag)
    return h256(tag_hash + tag_hash + message)


def schnorr_sign(secret: int, message: bytes) -> tuple[bytes, bytes]:
    """BIP340 signing with libsecp256k1's aux_rand32=null convention."""
    secret, public = normalize_scalar(secret)
    zero_mask = tagged_hash(b"BIP0340/aux", b"\0" * 32)
    masked = bytes(a ^ b for a, b in zip(b32(secret), zero_mask))
    nonce = int.from_bytes(tagged_hash(b"BIP0340/nonce", masked + b32(public[0]) + message), "big") % N
    if nonce == 0:
        raise ValueError("zero BIP340 nonce")
    r_point = point_mul(nonce)
    assert r_point is not None
    if r_point[1] & 1:
        nonce = N - nonce
        r_point = (r_point[0], P - r_point[1])
    challenge = int.from_bytes(tagged_hash(b"BIP0340/challenge", b32(r_point[0]) + b32(public[0]) + message), "big") % N
    signature = b32(r_point[0]) + b32((nonce + challenge * secret) % N)
    return b32(public[0]), signature


def schnorr_verify(public_x: bytes, message: bytes, signature: bytes) -> bool:
    if len(public_x) != 32 or len(message) != 32 or len(signature) != 64:
        return False
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:], "big")
    if r >= P or s >= N:
        return False
    try:
        public = lift_even_x(public_x)
        r_point = lift_even_x(signature[:32])
    except ValueError:
        return False
    challenge = int.from_bytes(
        tagged_hash(b"BIP0340/challenge", signature[:32] + public_x + message), "big") % N
    return point_mul(s) == point_add(r_point, point_mul(challenge, public))


def pedersen_zero_value_encoding(secret: int) -> bytes:
    point = point_mul(secret)
    assert point is not None
    is_square = pow(point[1], (P - 1) // 2, P) == 1
    return bytes([9 ^ int(is_square)]) + b32(point[0])


def binding_sighash(value_balance: int, tx_sighash: bytes,
                    spend_cvs: list[bytes], output_cvs: list[bytes]) -> bytes:
    material = b"DIN/v7/shielded/binding/v1" + struct.pack("<q", value_balance) + tx_sighash
    material += struct.pack("<Q", len(spend_cvs)) + b"".join(sorted(spend_cvs))
    material += struct.pack("<Q", len(output_cvs)) + b"".join(sorted(output_cvs))
    return h256(material)


def serialize_bundle(value_balance: int, spends: list[dict[str, bytes]],
                     outputs: list[dict[str, bytes]], range_proofs: bytes,
                     *, canonical_order: bool = True) -> bytes:
    if canonical_order:
        spends = sorted(spends, key=lambda item: item["nullifier"])
        outputs = sorted(outputs, key=lambda item: item["commitment"])
    out = struct.pack("<q", value_balance) + compact_size(len(spends))
    for spend in spends:
        out += spend["nullifier"] + spend["anchor"] + spend["cv"] + vec(spend["proof"])
    out += compact_size(len(outputs))
    for output in outputs:
        out += output["commitment"] + output["cv"] + vec(output["note"]) + vec(output["proof"])
    out += vec(range_proofs)
    out += bytes.fromhex("08" + "44" * 32)  # structurally valid-sized bvk container
    out += bytes.fromhex("55" * 64)
    return out


def serialize_v6_transaction(bundle: bytes, lock_time: int = 0) -> bytes:
    """Deterministic v6 envelope used only to pin transaction serialization.

    It deliberately includes one transparent input and output: a zero-input
    fixture begins with 00 00 after the version, which is ambiguous with the
    SegWit marker during parsing.  The bundle proof components remain
    serialization sentinels, so this is not represented as consensus-valid.
    """
    txin = (bytes(range(32)) + struct.pack("<I", 7) + vec(b"\x51\x21") +
            struct.pack("<I", 0xFFFFFFFD))
    txout = struct.pack("<Q", 12_345) + vec(b"\x6a\x01\x7f")
    return (struct.pack("<I", 6) + compact_size(1) + txin + compact_size(1) +
            txout + b"\0" + vec(bundle) + struct.pack("<I", lock_time))


def ipa_container(rounds: int, point_seed: int, scalar_seed: int) -> bytes:
    out = struct.pack(">I", rounds)
    out += b"".join(compressed_point(point_seed + i) for i in range(rounds))
    out += b"".join(compressed_point(point_seed + rounds + i) for i in range(rounds))
    out += b32(scalar_seed) + b32(scalar_seed + 1)
    return out


def hyrax_commitment(rows: int, cols: int, total: int, point_seed: int) -> bytes:
    return (struct.pack(">QQQ", rows, cols, total) +
            b"".join(compressed_point(point_seed + i) for i in range(rows)))


def hyrax_eval_container(claimed: int, rounds: int, point_seed: int,
                         scalar_seed: int) -> bytes:
    return b32(claimed) + ipa_container(rounds, point_seed, scalar_seed)


def spartan_container() -> bytes:
    """A structurally parseable Spartan/Hyrax encoding, not a valid proof."""
    out = hyrax_commitment(2, 4, 8, 2)
    out += hyrax_commitment(1, 2, 2, 10)
    out += h256(b"independent Spartan circuit identity")
    out += struct.pack(">Q", 2)
    for value in range(20, 28):
        out += b32(value)
    for value in range(30, 34):
        out += b32(value)
    out += struct.pack(">Q", 1)
    for value in range(40, 43):
        out += b32(value)
    out += hyrax_eval_container(50, 2, 20, 60)
    out += hyrax_eval_container(51, 1, 30, 70)
    return out


class Transcript:
    def __init__(self, label: str):
        self.state = bytearray()
        self.append("dom-sep", label.encode())

    def append(self, label: str, data: bytes) -> None:
        encoded = label.encode()
        self.state += bytes([len(encoded) & 0xFF]) + encoded + struct.pack(">I", len(data)) + data

    def scalar(self, label: str, value: bytes) -> None:
        self.append(label, value)

    def point(self, label: str, value: bytes) -> None:
        self.append(label, value)

    def u64(self, label: str, value: int) -> None:
        self.append(label, struct.pack(">Q", value))

    def challenge(self, label: str) -> bytes:
        encoded = label.encode()
        raw = h256(bytes(self.state) + b"chal" + encoded + bytes([len(encoded) & 0xFF]))
        self.state += raw
        return b32(valid_scalar(raw))


def transcript_vector(label: str, fields: list[tuple[str, str, object]]) -> dict[str, str]:
    transcript = Transcript(label)
    preamble = bytes(transcript.state)
    for kind, name, value in fields:
        if kind == "scalar":
            transcript.scalar(name, value)  # type: ignore[arg-type]
        elif kind == "point":
            transcript.point(name, value)  # type: ignore[arg-type]
        else:
            transcript.u64(name, value)  # type: ignore[arg-type]
    before = bytes(transcript.state)
    challenge = transcript.challenge("tau")
    return {
        "domain_frame": preamble.hex(),
        "statement_frame": before.hex(),
        "tau": challenge.hex(),
        "state_after_tau": bytes(transcript.state).hex(),
    }


def operation_vectors(note: bytes, commitment: bytes, nullifier: bytes) -> dict[str, object]:
    anchor = h256(b"DIN independent vector anchor")
    spend = {
        "nullifier": nullifier,
        "anchor": anchor,
        "cv": bytes.fromhex("08" + "22" * 32),
        "proof": b"\x03" + bytes.fromhex("a1b2c3d4"),
    }
    output = {
        "commitment": commitment,
        "cv": bytes.fromhex("09" + "33" * 32),
        "note": note,
        "proof": b"\x04" + bytes.fromhex("10203040"),
    }
    ranges = range_container([b"\xaa\xbb", b"\xcc"])
    shield = serialize_bundle(100_000_000, [], [output], range_container([b"\xcc"]))
    transfer = serialize_bundle(0, [spend], [output], ranges)
    unshield = serialize_bundle(-100_000_000, [spend], [], range_container([b"\xaa\xbb"]))
    high_spend = dict(spend)
    high_spend["nullifier"] = b"\xff" * 32
    low_spend = dict(spend)
    low_spend["nullifier"] = b"\0" * 32
    descending_spends = serialize_bundle(
        0, [high_spend, low_spend], [], range_container([b"\xaa", b"\xbb"]),
        canonical_order=False)
    return {
        "classification": (
            "canonical addressed v6 shielded-bundle and transaction bytes; "
            "proof bytes are serialization fixtures, not valid proofs"),
        "shield": shield.hex(),
        "transfer": transfer.hex(),
        "unshield": unshield.hex(),
        "full_transactions": {
            "shield": serialize_v6_transaction(shield, 0x01020304).hex(),
            "transfer": serialize_v6_transaction(transfer, 0x11223344).hex(),
            "unshield": serialize_v6_transaction(unshield, 0xA0B0C0D0).hex(),
        },
        "range_container": ranges.hex(),
        "range_compactsize_boundaries": {
            "proof_252_bytes": range_container([b"\xa5" * 252]).hex(),
            "proof_253_bytes": range_container([b"\x5a" * 253]).hex(),
        },
        "canonical_rejections": {
            "trailing_byte": (shield + b"\0").hex(),
            "truncated": shield[:-1].hex(),
            "nonminimal_spend_count": (shield[:8] + b"\xfd\x00\x00" + shield[9:]).hex(),
            "descending_spends": descending_spends.hex(),
            "expected": {
                "trailing_byte": "TrailingBytes",
                "truncated": "Truncated",
                "nonminimal_spend_count": "Truncated",
                "descending_spends": "OrderViolation",
            },
        },
    }


def build_vectors() -> dict[str, object]:
    seed = canonical_seed()
    keys = derive_account(seed, 0)
    address = derive_address(keys, 0)
    note = note_vector(keys, address)

    d_packed = address["d"].ljust(32, b"\0")  # type: ignore[union-attr]
    pk_note = poseidon(note["rcm"], dst("DIN/v7/shielded/sk_note/v1"))
    pk_note = poseidon(pk_note, b"\0" * 32)
    addr_key = poseidon(d_packed, pk_note)
    addr_bind = poseidon(dst("DIN/v7/shielded/addr/v1"), addr_key)
    value_field = b32(100_000_000)
    commitment = poseidon(poseidon(addr_bind, value_field), note["rcm"])
    sk_note = poseidon(note["rcm"], dst("DIN/v7/shielded/sk_note/v1"))
    nullifier_0 = poseidon(sk_note, b32(0))
    nullifier_max = poseidon(sk_note, b32(0xFFFFFFFFFFFFFFFF))

    zero_out, zero_trace = poseidon_trace(0, 0)
    seq_out, seq_trace = poseidon_trace(1, 2)

    spend_base = [("scalar", "nf", nullifier_0), ("scalar", "an", h256(b"anchor"))]
    cv = bytes.fromhex("08" + "66" * 32)
    output_base = [("scalar", "cm", commitment)]
    transcripts = {
        "spend_v01_legacy": transcript_vector("dinero.shielded.spend.v1", spend_base),
        "output_v02_legacy": transcript_vector("dinero.shielded.output.v1", output_base),
        "spend_v03_cv": transcript_vector(
            "dinero.shielded.spend.v1",
            spend_base + [("u64", "cv0", cv[0]), ("scalar", "cvx", cv[1:])]),
        "output_v04_cv": transcript_vector(
            "dinero.shielded.output.v1",
            output_base + [("u64", "cv0", cv[0]), ("scalar", "cvx", cv[1:])]),
        "spend_v05_auth_dormant": transcript_vector(
            "dinero.shielded.spend.v1",
            spend_base + [("u64", "cv0", cv[0]), ("scalar", "cvx", cv[1:]),
                          ("u64", "auth", 1)]),
    }

    tx_envelope = (struct.pack("<i", 6) + struct.pack("<I", 0) + struct.pack("<I", 1) +
                   struct.pack("<Q", 123_456_789) + struct.pack("<I", 3) + b"\x6a\x01\x42" +
                   struct.pack("<I", 0x11223344))
    tx_sighash = h256(b"DIN/v7/shielded/tx-sighash/v1" + tx_envelope)
    binding_spends = [bytes.fromhex("09" + "22" * 32), bytes.fromhex("08" + "11" * 32)]
    binding_outputs = [bytes.fromhex("08" + "33" * 32)]
    bind_hash = binding_sighash(-42, tx_sighash, binding_spends, binding_outputs)
    binding_secret = int.from_bytes(bytes.fromhex("1f" * 32), "big")
    binding_public, binding_signature = schnorr_sign(binding_secret, bind_hash)
    binding = {
        "tx_envelope_preimage": tx_envelope,
        "tx_sighash": tx_sighash,
        "value_balance": -42,
        "spend_cvs_unsorted": binding_spends,
        "output_cvs": binding_outputs,
        "binding_sighash": bind_hash,
        "bsk": b32(binding_secret),
        "bvk_xonly": binding_public,
        "bvk_commitment": pedersen_zero_value_encoding(binding_secret),
        "signature": binding_signature,
    }

    leaves = [commitment, h256(b"second independent note")]
    empty = poseidon(b"\0" * 32, b"\0" * 32)
    empty_roots = [empty]
    for _ in range(32):
        empty_roots.append(poseidon(empty_roots[-1], empty_roots[-1]))

    def root(items: list[bytes]) -> bytes:
        frontier = [b"\0" * 32 for _ in range(32)]
        size = 0
        for leaf in items:
            current, index = leaf, size
            for depth in range(32):
                if index & 1 == 0:
                    frontier[depth] = current
                    break
                current = poseidon(frontier[depth], current)
                index >>= 1
            size += 1
        current = empty_roots[0]
        for depth in range(32):
            current = (poseidon(frontier[depth], current)
                       if (size >> depth) & 1
                       else poseidon(current, empty_roots[depth]))
        return current

    operations = operation_vectors(note["encrypted_note"], commitment, nullifier_0)

    def jsonify(value: object) -> object:
        if isinstance(value, bytes):
            return value.hex()
        if isinstance(value, list):
            return [jsonify(item) for item in value]
        if isinstance(value, dict):
            return {key: jsonify(item) for key, item in value.items()}
        return value

    result = {
        "format": "dinero-shielded-protocol-v1-independent-vectors",
        "version": 1,
        "oracle": "Python 3 standard library; no Dinero code or linked crypto library",
        "scope": {
            "independent": [
                "Poseidon round constants/intermediate states", "BIP32 key derivation",
                "diversifiers/hash-to-point/addresses", "note commitments/nullifiers",
                "recipient note encryption/decryption bytes", "Fiat-Shamir transcript framing",
                "Spartan/Hyrax proof-container serialization", "binding sighashes and signatures",
                "range-proof container and bundle serialization", "canonical parser rejection fixtures",
                "shield/transfer/unshield full v6 transaction bytes", "commitment-tree epoch/reset transitions",
            ],
            "not_claimed": [
                "independent Spartan/Hyrax prover", "independent Borromean range-proof prover",
                "Gate E snapshot-section or coinbase-Merkle binding",
                "outgoing-view recovery (not implemented by protocol)",
            ],
        },
        "poseidon": {
            "modulus": f"{N:064x}",
            "round_constants": [[f"{item:064x}" for item in row] for row in POSEIDON_CONSTANTS],
            "zero_zero": {
                "output": f"{zero_out:064x}",
                "states": [[f"{item:064x}" for item in row] for row in zero_trace],
            },
            "one_two": {"output": f"{seq_out:064x}", "states": [[f"{item:064x}" for item in row] for row in seq_trace]},
        },
        "derivation": {"seed": seed, "account": 0, **keys, "address_j0": address},
        "note": note,
        "commitment": {
            "d_packed": d_packed, "sk_note": sk_note, "pk_note": pk_note,
            "addr_key": addr_key, "addr_bind": addr_bind, "value_be32": value_field,
            "note_commitment": commitment, "nullifier_leaf_0": nullifier_0,
            "nullifier_leaf_u64max": nullifier_max,
        },
        "transcripts": transcripts,
        "binding": binding,
        "proof_serialization": {
            "hyrax_commitment_synthetic": hyrax_commitment(2, 4, 8, 2).hex(),
            "hyrax_eval_synthetic": hyrax_eval_container(50, 2, 20, 60).hex(),
            "spartan_synthetic": spartan_container().hex(),
            "coverage": (
                "parseable encoding and transcript interoperability only; "
                "synthetic proof components are not validity claims"),
        },
        "operations": operations,
        "epoch_reset": {
            "empty_root": root([]), "root_after_first": root(leaves[:1]),
            "root_after_second": root(leaves), "root_after_truncate_to_one": root(leaves[:1]),
            "root_after_reset": root([]), "root_after_reapply": root(leaves),
            "state_before_reset": {
                "tree_size": 2,
                "nullifiers": [
                    {"height": 10, "value": nullifier_0},
                    {"height": 11, "value": nullifier_max},
                ],
                "anchor_height": 11,
            },
            "state_after_reset": {
                "tree_size": 0,
                "nullifier_count": 0,
                "old_anchor_present": False,
            },
        },
    }
    return jsonify(result)  # type: ignore[return-value]


def canonical_json(vectors: dict[str, object]) -> str:
    return json.dumps(vectors, indent=2, sort_keys=True) + "\n"


def self_test(vectors: dict[str, object]) -> None:
    baseline = canonical_json(vectors)
    mutants = []
    for path, replacement in [
        (("poseidon", "zero_zero", "output"), "00" * 32),
        (("derivation", "address_j0", "mainnet"), "dins1mutated"),
        (("note", "tag"), "00" * 16),
        (("commitment", "nullifier_leaf_0"), "ff" * 32),
        (("operations", "range_container"), "00"),
        (("epoch_reset", "root_after_reapply"), "11" * 32),
    ]:
        mutant = json.loads(baseline)
        cursor = mutant
        for component in path[:-1]:
            cursor = cursor[component]
        cursor[path[-1]] = replacement
        mutants.append(canonical_json(mutant))
    if any(mutant == baseline for mutant in mutants):
        raise AssertionError("mutation control failed to alter canonical vectors")
    if len(set(mutants)) != len(mutants):
        raise AssertionError("mutation controls collided")

    note = vectors["note"]
    assert isinstance(note, dict)
    tag = bytearray.fromhex(str(note["tag"]))
    tag[0] ^= 1
    try:
        aead_decrypt(
            bytes.fromhex(str(note["aead_key"])), b"\0" * 12,
            bytes.fromhex(str(note["epk"])),
            bytes.fromhex(str(note["ciphertext"])), bytes(tag))
    except ValueError:
        pass
    else:
        raise AssertionError("mutated recipient-note tag was accepted")

    binding = vectors["binding"]
    assert isinstance(binding, dict)
    public_x = bytes.fromhex(str(binding["bvk_xonly"]))
    message = bytes.fromhex(str(binding["binding_sighash"]))
    signature = bytes.fromhex(str(binding["signature"]))
    if not schnorr_verify(public_x, message, signature):
        raise AssertionError("independent binding signature did not verify")
    changed_message = bytearray(message)
    changed_message[0] ^= 1
    if schnorr_verify(public_x, bytes(changed_message), signature):
        raise AssertionError("binding signature accepted a mutated sighash")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path("tests/vectors/shielded_protocol_v1_external.json"))
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    vectors = build_vectors()
    rendered = canonical_json(vectors)
    if args.self_test:
        self_test(vectors)
    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"missing vector file: {args.output}", file=sys.stderr)
            return 1
        if existing != rendered:
            print(f"shielded protocol vectors differ: regenerate {args.output}", file=sys.stderr)
            return 1
        print(f"PASS: independent shielded vectors match {args.output}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
