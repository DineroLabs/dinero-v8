"""
DineroCoin Test Vector Generation

Generates deterministic, reproducible test vectors for C++ tests.
Outputs JSON that can be loaded by gtest or integration tests.
"""

import json
import time
from pathlib import Path
from typing import Dict, List, Any, Optional
from dataclasses import asdict

from .params import get_network, UNA_PER_DIN
from .pow import DineroHeader, mine_header_chain, double_sha256, bits_to_target
from .merkle import merkle_root, merkle_root_hex
from .tx import Transaction, TxInput, TxOutput, OutPoint, create_coinbase_tx
from .keys import generate_keypair, keypair_from_privkey, sign_schnorr, sign_ecdsa
from .script import p2pkh_from_pubkey, p2wpkh_from_pubkey, p2tr_script

# =============================================================================
# Vector File Management
# =============================================================================

def write_vectors(path: str, vectors: Dict[str, Any], pretty: bool = True):
    """Write vectors to JSON file."""
    output = {
        "generated_at": int(time.time()),
        "generator": "dinero_test_utils",
        "version": "1.0.0",
        "vectors": vectors,
    }

    with open(path, 'w') as f:
        if pretty:
            json.dump(output, f, indent=2)
        else:
            json.dump(output, f)

def load_vectors(path: str) -> Dict[str, Any]:
    """Load vectors from JSON file."""
    with open(path, 'r') as f:
        data = json.load(f)
    return data.get("vectors", data)

# =============================================================================
# Header Vectors
# =============================================================================

def generate_header_vectors(count: int = 4,
                            network: str = "regtest",
                            base_timestamp: int = 1_000_000) -> Dict[str, Any]:
    """
    Generate mined header chain vectors.

    Returns dict with:
        - network params used
        - list of headers with nonces and hashes
        - C++ code snippet for embedding
    """
    params = get_network(network)

    headers = mine_header_chain(
        count=count,
        bits=params.bits,
        base_timestamp=base_timestamp,
        verbose=True,
    )

    vectors = {
        "network": network,
        "bits": f"0x{params.bits:08x}",
        "bits_int": params.bits,
        "target": f"0x{bits_to_target(params.bits):064x}",
        "count": count,
        "headers": [],
    }

    cpp_lines = [
        f"// Pre-computed header vectors for {network}",
        f"// Bits: 0x{params.bits:08x}",
        "",
    ]

    for i, (header, nonce, hash_hex) in enumerate(headers):
        height = i + 1
        header_data = {
            "height": height,
            "version": header.version,
            "prev_block_hash": header.prev_block_hash[::-1].hex(),  # Display format
            "merkle_root": header.merkle_root[::-1].hex(),
            "timestamp": header.timestamp,
            "bits": header.bits,
            "nonce": nonce,
            "utreexo_root": header.utreexo_root[::-1].hex(),
            "hash": hash_hex,
            "header_hex": header.serialize().hex(),
        }
        vectors["headers"].append(header_data)

        cpp_lines.append(f"// Header {height}")
        cpp_lines.append(f"static constexpr uint32_t HEADER_{height}_NONCE = {nonce};")
        cpp_lines.append(f"static constexpr uint32_t HEADER_{height}_TIMESTAMP = {header.timestamp};")
        cpp_lines.append(f'// Hash: {hash_hex[:16]}...')
        cpp_lines.append("")

    vectors["cpp_snippet"] = "\n".join(cpp_lines)

    return vectors

# =============================================================================
# Merkle Vectors
# =============================================================================

def generate_merkle_vectors() -> Dict[str, Any]:
    """
    Generate merkle root test vectors.

    Includes:
        - Single tx (root = txid)
        - Multiple txs
        - Odd number of txs (last duplicated)
        - Merkle proofs
    """
    vectors = {
        "description": "Merkle root computation test vectors",
        "test_cases": [],
    }

    # Case 1: Single transaction
    txid1 = double_sha256(b"tx1")
    root1 = merkle_root([txid1])
    vectors["test_cases"].append({
        "name": "single_tx",
        "txids": [txid1[::-1].hex()],
        "expected_root": root1[::-1].hex(),
        "note": "Single tx: root == txid",
    })

    # Case 2: Two transactions
    txid2 = double_sha256(b"tx2")
    root2 = merkle_root([txid1, txid2])
    vectors["test_cases"].append({
        "name": "two_txs",
        "txids": [txid1[::-1].hex(), txid2[::-1].hex()],
        "expected_root": root2[::-1].hex(),
    })

    # Case 3: Three transactions (odd, last duplicated)
    txid3 = double_sha256(b"tx3")
    root3 = merkle_root([txid1, txid2, txid3])
    vectors["test_cases"].append({
        "name": "three_txs_odd",
        "txids": [txid1[::-1].hex(), txid2[::-1].hex(), txid3[::-1].hex()],
        "expected_root": root3[::-1].hex(),
        "note": "Odd count: last txid duplicated",
    })

    # Case 4: Four transactions
    txid4 = double_sha256(b"tx4")
    root4 = merkle_root([txid1, txid2, txid3, txid4])
    vectors["test_cases"].append({
        "name": "four_txs",
        "txids": [
            txid1[::-1].hex(), txid2[::-1].hex(),
            txid3[::-1].hex(), txid4[::-1].hex(),
        ],
        "expected_root": root4[::-1].hex(),
    })

    # Case 5: Empty (edge case)
    root_empty = merkle_root([])
    vectors["test_cases"].append({
        "name": "empty",
        "txids": [],
        "expected_root": root_empty[::-1].hex(),
        "note": "Empty list returns 32 zero bytes",
    })

    return vectors

# =============================================================================
# Transaction Vectors
# =============================================================================

def generate_tx_vectors() -> Dict[str, Any]:
    """
    Generate transaction serialization test vectors.

    Includes:
        - Coinbase transaction
        - Simple P2PKH spend
        - SegWit P2WPKH spend
        - Taproot P2TR spend
    """
    vectors = {
        "description": "Transaction serialization test vectors",
        "test_cases": [],
    }

    # Generate test keys
    test_privkey = bytes.fromhex("0000000000000000000000000000000000000000000000000000000000000001")
    keypair = keypair_from_privkey(test_privkey)

    # Case 1: Coinbase transaction
    coinbase = create_coinbase_tx(
        height=1,
        value=50 * UNA_PER_DIN,
        script_pubkey=p2pkh_from_pubkey(keypair.public_key),
        extra_data=b"DineroCoin Regtest",
    )
    vectors["test_cases"].append({
        "name": "coinbase_height_1",
        "description": "Coinbase at height 1 with 50 DIN reward",
        "raw_hex": coinbase.serialize().hex(),
        "txid": coinbase.get_txid_hex(),
        "wtxid": coinbase.get_wtxid_hex(),
        "version": coinbase.version,
        "num_inputs": len(coinbase.inputs),
        "num_outputs": len(coinbase.outputs),
        "output_value": coinbase.outputs[0].value,
        "vsize": coinbase.get_virtual_size(),
    })

    # Case 2: Simple spend (non-witness)
    spend_tx = Transaction(
        version=2,
        inputs=[
            TxInput(
                prevout=OutPoint(
                    txid=coinbase.get_txid(),
                    vout=0,
                ),
                script_sig=bytes(72),  # Placeholder DER signature length
                sequence=0xFFFFFFFD,
            ),
        ],
        outputs=[
            TxOutput(
                value=40 * UNA_PER_DIN,
                script_pubkey=p2pkh_from_pubkey(keypair.public_key),
            ),
            TxOutput(
                value=9 * UNA_PER_DIN,  # Change
                script_pubkey=p2pkh_from_pubkey(keypair.public_key),
            ),
        ],
        locktime=0,
    )

    vectors["test_cases"].append({
        "name": "simple_p2pkh_spend",
        "description": "P2PKH spend with change output",
        "note": "scriptSig is placeholder - use for serialization testing only",
        "raw_hex": spend_tx.serialize(include_witness=False).hex(),
        "txid": spend_tx.get_txid_hex(),
        "version": spend_tx.version,
        "num_inputs": len(spend_tx.inputs),
        "num_outputs": len(spend_tx.outputs),
        "total_output": sum(out.value for out in spend_tx.outputs),
        "vsize": spend_tx.get_virtual_size(),
    })

    # Case 3: SegWit P2WPKH spend
    segwit_tx = Transaction(
        version=2,
        inputs=[
            TxInput(
                prevout=OutPoint(
                    txid=coinbase.get_txid(),
                    vout=0,
                ),
                script_sig=b'',  # Empty for native segwit
                sequence=0xFFFFFFFD,
                witness=[
                    bytes(72),  # Signature placeholder
                    keypair.public_key,
                ],
            ),
        ],
        outputs=[
            TxOutput(
                value=49 * UNA_PER_DIN,
                script_pubkey=p2wpkh_from_pubkey(keypair.public_key),
            ),
        ],
        locktime=0,
    )

    vectors["test_cases"].append({
        "name": "segwit_p2wpkh_spend",
        "description": "Native SegWit P2WPKH spend",
        "raw_hex": segwit_tx.serialize(include_witness=True).hex(),
        "raw_hex_no_witness": segwit_tx.serialize(include_witness=False).hex(),
        "txid": segwit_tx.get_txid_hex(),
        "wtxid": segwit_tx.get_wtxid_hex(),
        "has_witness": segwit_tx.has_witness(),
        "vsize": segwit_tx.get_virtual_size(),
    })

    # Case 4: Taproot P2TR spend
    taproot_tx = Transaction(
        version=2,
        inputs=[
            TxInput(
                prevout=OutPoint(
                    txid=coinbase.get_txid(),
                    vout=0,
                ),
                script_sig=b'',
                sequence=0xFFFFFFFD,
                witness=[
                    bytes(64),  # 64-byte Schnorr signature placeholder
                ],
            ),
        ],
        outputs=[
            TxOutput(
                value=49 * UNA_PER_DIN,
                script_pubkey=p2tr_script(keypair.x_only_pubkey),
            ),
        ],
        locktime=0,
    )

    vectors["test_cases"].append({
        "name": "taproot_p2tr_spend",
        "description": "Taproot key-path spend",
        "output_script": p2tr_script(keypair.x_only_pubkey).hex(),
        "x_only_pubkey": keypair.x_only_pubkey.hex(),
        "raw_hex": taproot_tx.serialize(include_witness=True).hex(),
        "txid": taproot_tx.get_txid_hex(),
        "wtxid": taproot_tx.get_wtxid_hex(),
        "vsize": taproot_tx.get_virtual_size(),
    })

    return vectors

# =============================================================================
# Signature Vectors
# =============================================================================

def generate_signature_vectors() -> Dict[str, Any]:
    """
    Generate ECDSA and Schnorr signature test vectors.
    """
    vectors = {
        "description": "Signature test vectors",
        "test_cases": [],
    }

    # Test key (NEVER use for real funds!)
    test_privkey = bytes.fromhex("0000000000000000000000000000000000000000000000000000000000000001")
    keypair = keypair_from_privkey(test_privkey)

    # Test message
    message = double_sha256(b"test message for signing")

    # ECDSA signature
    ecdsa_sig = sign_ecdsa(test_privkey, message)
    vectors["test_cases"].append({
        "name": "ecdsa_deterministic",
        "type": "ecdsa",
        "private_key": test_privkey.hex(),
        "public_key": keypair.public_key.hex(),
        "message": message.hex(),
        "signature_der": ecdsa_sig.hex(),
        "note": "RFC6979 deterministic k",
    })

    # Schnorr signature
    schnorr_sig = sign_schnorr(test_privkey, message)
    vectors["test_cases"].append({
        "name": "schnorr_bip340",
        "type": "schnorr",
        "private_key": test_privkey.hex(),
        "x_only_pubkey": keypair.x_only_pubkey.hex(),
        "message": message.hex(),
        "signature": schnorr_sig.hex(),
        "note": "BIP340 Schnorr signature",
    })

    return vectors

# =============================================================================
# All-in-One Generator
# =============================================================================

def generate_all_vectors(output_dir: str = "tests/vectors",
                         network: str = "regtest"):
    """
    Generate all test vector files.
    """
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    print(f"Generating test vectors for {network}...")
    print(f"Output directory: {output_path}")
    print()

    # Headers
    print("Generating header vectors...")
    header_vectors = generate_header_vectors(count=4, network=network)
    write_vectors(output_path / f"headers_{network}.json", header_vectors)
    print(f"  -> headers_{network}.json")

    # Print C++ snippet
    print("\nC++ snippet for embedding:")
    print("-" * 40)
    print(header_vectors["cpp_snippet"])
    print("-" * 40)
    print()

    # Merkle
    print("Generating merkle vectors...")
    merkle_vectors = generate_merkle_vectors()
    write_vectors(output_path / "merkle_vectors.json", merkle_vectors)
    print("  -> merkle_vectors.json")

    # Transactions
    print("Generating transaction vectors...")
    tx_vectors = generate_tx_vectors()
    write_vectors(output_path / "tx_vectors.json", tx_vectors)
    print("  -> tx_vectors.json")

    # Signatures
    print("Generating signature vectors...")
    sig_vectors = generate_signature_vectors()
    write_vectors(output_path / "signature_vectors.json", sig_vectors)
    print("  -> signature_vectors.json")

    print()
    print("Done!")
