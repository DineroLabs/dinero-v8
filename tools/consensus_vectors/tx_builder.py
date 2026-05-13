"""
DineroCoin Transaction Builder

Builds valid and invalid transactions for consensus testing.
"""

import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional, Dict, Any, Tuple

sys.path.insert(0, str(Path(__file__).parent.parent))

from regtest_crypto.tx import (
    Transaction, TxInput, TxOutput, OutPoint,
    compact_size, SEQUENCE_FINAL,
)
from regtest_crypto.keys import generate_keypair, sign_ecdsa
from regtest_crypto.script import p2pkh_from_pubkey, p2pkh_script
from regtest_crypto.pow import double_sha256

from .rules import ConsensusRules


class TxBuilder:
    """
    Builds valid transactions for testing.
    """

    def __init__(self):
        pass

    def create_simple_tx(
        self,
        inputs: List[Tuple[bytes, int, int]],  # (txid, vout, amount)
        outputs: List[Tuple[bytes, int]],      # (scriptPubKey, amount)
        privkeys: List[bytes],                  # Private keys to sign
    ) -> Transaction:
        """
        Create a simple signed transaction.

        Args:
            inputs: List of (txid, vout, input_amount)
            outputs: List of (scriptPubKey, amount)
            privkeys: Private keys for signing (one per input)

        Returns:
            Signed transaction
        """
        tx = Transaction(version=2, locktime=0)

        # Add inputs
        for txid, vout, _ in inputs:
            tx.inputs.append(TxInput(
                prevout=OutPoint(txid=txid, vout=vout),
                script_sig=b'',
                sequence=SEQUENCE_FINAL,
            ))

        # Add outputs
        for script, amount in outputs:
            tx.outputs.append(TxOutput(
                value=amount,
                script_pubkey=script,
            ))

        # Sign each input (simplified P2PKH signing)
        for i, privkey in enumerate(privkeys):
            # Compute sighash
            sighash = tx.sighash_legacy(i, inputs[i][2])  # Pass input amount for reference
            # Sign
            sig = sign_ecdsa(privkey, sighash)
            # Build scriptSig (sig + pubkey)
            from regtest_crypto.keys import keypair_from_privkey
            _, pubkey = keypair_from_privkey(privkey)
            tx.inputs[i].script_sig = (
                bytes([len(sig) + 1]) + sig + bytes([0x01]) +  # sig + SIGHASH_ALL
                bytes([len(pubkey)]) + pubkey
            )

        return tx

    def create_multisig_tx(
        self,
        inputs: List[Tuple[bytes, int, int]],
        outputs: List[Tuple[bytes, int]],
        m: int,
        pubkeys: List[bytes],
        privkeys: List[bytes],
    ) -> Transaction:
        """Create an m-of-n multisig transaction"""
        # TODO: Implement multisig
        raise NotImplementedError("Multisig not yet implemented")


class InvalidTxBuilder(TxBuilder):
    """
    Builds intentionally-invalid transactions for negative testing.
    """

    def bad_no_inputs(self) -> Tuple[Transaction, str]:
        """Transaction with no inputs. Violates: TX_NOT_EMPTY"""
        tx = Transaction(version=2, locktime=0)
        tx.outputs.append(TxOutput(
            value=1_000_000,
            script_pubkey=b'\x76\xa9\x14' + b'\x00' * 20 + b'\x88\xac',
        ))
        return tx, "TX_NOT_EMPTY"

    def bad_no_outputs(
        self,
        input_txid: bytes,
        input_vout: int,
    ) -> Tuple[Transaction, str]:
        """Transaction with no outputs. Violates: TX_NOT_EMPTY"""
        tx = Transaction(version=2, locktime=0)
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=input_txid, vout=input_vout),
            script_sig=b'\x00',
            sequence=SEQUENCE_FINAL,
        ))
        return tx, "TX_NOT_EMPTY"

    def bad_duplicate_inputs(
        self,
        input_txid: bytes,
        input_vout: int,
        output_script: bytes,
        output_amount: int,
    ) -> Tuple[Transaction, str]:
        """Transaction spending same input twice. Violates: NO_DUP_INPUTS"""
        tx = Transaction(version=2, locktime=0)

        # Add same input twice
        for _ in range(2):
            tx.inputs.append(TxInput(
                prevout=OutPoint(txid=input_txid, vout=input_vout),
                script_sig=b'\x00',
                sequence=SEQUENCE_FINAL,
            ))

        tx.outputs.append(TxOutput(
            value=output_amount,
            script_pubkey=output_script,
        ))

        return tx, "NO_DUP_INPUTS"

    def bad_negative_output(
        self,
        input_txid: bytes,
        input_vout: int,
    ) -> Tuple[Transaction, str]:
        """Transaction with negative output value. Violates: OUTPUT_VALUE"""
        tx = Transaction(version=2, locktime=0)
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=input_txid, vout=input_vout),
            script_sig=b'\x00',
            sequence=SEQUENCE_FINAL,
        ))
        tx.outputs.append(TxOutput(
            value=-1,  # Negative!
            script_pubkey=b'\x76\xa9\x14' + b'\x00' * 20 + b'\x88\xac',
        ))
        return tx, "OUTPUT_VALUE"

    def bad_overflow_output(
        self,
        input_txid: bytes,
        input_vout: int,
    ) -> Tuple[Transaction, str]:
        """Transaction with output exceeding 21M. Violates: OUTPUT_VALUE"""
        tx = Transaction(version=2, locktime=0)
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=input_txid, vout=input_vout),
            script_sig=b'\x00',
            sequence=SEQUENCE_FINAL,
        ))
        tx.outputs.append(TxOutput(
            value=21_000_001 * 100_000_000,  # >21M DIN
            script_pubkey=b'\x76\xa9\x14' + b'\x00' * 20 + b'\x88\xac',
        ))
        return tx, "OUTPUT_VALUE"

    def bad_output_sum_overflow(
        self,
        input_txid: bytes,
        input_vout: int,
    ) -> Tuple[Transaction, str]:
        """Transaction where output sum overflows. Violates: OUTPUT_SUM"""
        tx = Transaction(version=2, locktime=0)
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=input_txid, vout=input_vout),
            script_sig=b'\x00',
            sequence=SEQUENCE_FINAL,
        ))

        # Add outputs that would overflow when summed
        max_money = 21_000_000 * 100_000_000
        tx.outputs.append(TxOutput(value=max_money, script_pubkey=b'\x00'))
        tx.outputs.append(TxOutput(value=max_money, script_pubkey=b'\x00'))

        return tx, "OUTPUT_SUM"

    def bad_missing_input(
        self,
        output_script: bytes,
        output_amount: int,
    ) -> Tuple[Transaction, str]:
        """Transaction referencing non-existent UTXO. Violates: UTXO_EXISTS"""
        tx = Transaction(version=2, locktime=0)

        # Reference a UTXO that doesn't exist
        fake_txid = double_sha256(b"this transaction does not exist")
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=fake_txid, vout=0),
            script_sig=b'\x00',
            sequence=SEQUENCE_FINAL,
        ))

        tx.outputs.append(TxOutput(
            value=output_amount,
            script_pubkey=output_script,
        ))

        return tx, "UTXO_EXISTS"

    def bad_script_invalid(
        self,
        input_txid: bytes,
        input_vout: int,
        output_script: bytes,
        output_amount: int,
    ) -> Tuple[Transaction, str]:
        """Transaction with invalid scriptSig. Violates: SCRIPT_VALID"""
        tx = Transaction(version=2, locktime=0)
        tx.inputs.append(TxInput(
            prevout=OutPoint(txid=input_txid, vout=input_vout),
            script_sig=b'\x00\x00',  # Invalid signature
            sequence=SEQUENCE_FINAL,
        ))
        tx.outputs.append(TxOutput(
            value=output_amount,
            script_pubkey=output_script,
        ))
        return tx, "SCRIPT_VALID"

    def generate_all_invalid(
        self,
        sample_txid: bytes,
        sample_vout: int = 0,
        sample_script: bytes = b'\x00\x14' + b'\x00' * 20,
        sample_amount: int = 1_000_000,
    ) -> List[Tuple[Transaction, str, str]]:
        """
        Generate all types of invalid transactions.

        Returns: List of (tx, rule_violated, description)
        """
        results = []

        generators = [
            (lambda: self.bad_no_inputs(), "Transaction with no inputs"),
            (lambda: self.bad_no_outputs(sample_txid, sample_vout), "Transaction with no outputs"),
            (lambda: self.bad_duplicate_inputs(sample_txid, sample_vout, sample_script, sample_amount),
             "Transaction with duplicate inputs"),
            (lambda: self.bad_negative_output(sample_txid, sample_vout), "Transaction with negative output"),
            (lambda: self.bad_overflow_output(sample_txid, sample_vout), "Transaction with overflow output"),
            (lambda: self.bad_missing_input(sample_script, sample_amount), "Transaction spending non-existent UTXO"),
            (lambda: self.bad_script_invalid(sample_txid, sample_vout, sample_script, sample_amount),
             "Transaction with invalid script"),
        ]

        for gen_func, description in generators:
            try:
                tx, rule = gen_func()
                results.append((tx, rule, description))
            except Exception as e:
                print(f"Warning: Generator failed: {e}")

        return results
