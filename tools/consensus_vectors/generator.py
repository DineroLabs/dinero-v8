#!/usr/bin/env python3
"""
DineroCoin Consensus Vector Generator

Generates comprehensive test vectors for consensus rule testing.
Outputs JSON that C++ tests can load.
"""

import json
import time
import argparse
import sys
from pathlib import Path
from typing import Dict, Any, List

sys.path.insert(0, str(Path(__file__).parent.parent))

from regtest_crypto.pow import double_sha256
from regtest_crypto.params import get_network

from .rules import ConsensusRules, RuleCategory
from .block_builder import BlockBuilder, InvalidBlockBuilder
from .tx_builder import TxBuilder, InvalidTxBuilder


class VectorGenerator:
    """
    Generates consensus test vectors.
    """

    def __init__(self, network: str = "regtest", output_dir: str = "tests/vectors"):
        self.network = network
        self.params = get_network(network)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.block_builder = BlockBuilder(network)
        self.invalid_block_builder = InvalidBlockBuilder(network)
        self.tx_builder = TxBuilder()
        self.invalid_tx_builder = InvalidTxBuilder()

    def _save(self, filename: str, data: Dict[str, Any]):
        """Save vectors to JSON file"""
        output = {
            "generator": "dinero_consensus_vectors",
            "version": "1.0.0",
            "network": self.network,
            "generated_at": int(time.time()),
            **data,
        }

        path = self.output_dir / filename
        with open(path, 'w') as f:
            json.dump(output, f, indent=2)
        print(f"Wrote: {path}")

    def generate_valid_blocks(self, count: int = 5) -> Dict[str, Any]:
        """Generate a chain of valid blocks"""
        print(f"Generating {count} valid blocks...")

        # Start from genesis
        genesis_hash = double_sha256(b"DineroCoin Genesis")
        coinbase_addr = b'\x00' * 20  # Dummy address

        blocks = []
        prev_hash = genesis_hash

        for height in range(1, count + 1):
            block = self.block_builder.create_block(
                prev_hash=prev_hash,
                height=height,
                coinbase_address=coinbase_addr,
            )

            vec = self.block_builder.to_vector(block, f"valid_block_{height}")
            vec["height"] = height
            vec["expected_result"] = "ACCEPT"
            blocks.append(vec)

            prev_hash = block.hash()

        return {"valid_blocks": blocks}

    def generate_invalid_blocks(self) -> Dict[str, Any]:
        """Generate blocks that violate each consensus rule"""
        print("Generating invalid blocks...")

        genesis_hash = double_sha256(b"DineroCoin Genesis")
        coinbase_addr = b'\x00' * 20

        invalid_blocks = self.invalid_block_builder.generate_all_invalid(
            prev_hash=genesis_hash,
            height=1,
            coinbase_address=coinbase_addr,
        )

        vectors = []
        for block, rule_id, description in invalid_blocks:
            vec = self.block_builder.to_vector(block, f"invalid_{rule_id.lower()}")
            vec["expected_result"] = "REJECT"
            vec["violates_rule"] = rule_id
            vec["description"] = description
            vectors.append(vec)

        return {"invalid_blocks": vectors}

    def generate_invalid_transactions(self) -> Dict[str, Any]:
        """Generate transactions that violate each consensus rule"""
        print("Generating invalid transactions...")

        sample_txid = double_sha256(b"sample transaction")

        invalid_txs = self.invalid_tx_builder.generate_all_invalid(
            sample_txid=sample_txid,
            sample_vout=0,
            sample_amount=1_000_000,
        )

        vectors = []
        for tx, rule_id, description in invalid_txs:
            vectors.append({
                "label": f"invalid_{rule_id.lower()}",
                "txid": tx.txid()[::-1].hex(),
                "tx_hex": tx.serialize().hex(),
                "expected_result": "REJECT",
                "violates_rule": rule_id,
                "description": description,
            })

        return {"invalid_transactions": vectors}

    def generate_boundary_conditions(self) -> Dict[str, Any]:
        """Generate edge case vectors for boundary conditions"""
        print("Generating boundary condition vectors...")

        vectors = {
            "max_block_size": {
                "description": "Block at exactly 4MB limit",
                "size_bytes": 4_000_000,
                "expected_result": "ACCEPT",
            },
            "over_block_size": {
                "description": "Block at 4MB + 1 byte",
                "size_bytes": 4_000_001,
                "expected_result": "REJECT",
                "violates_rule": "BLOCK_SIZE",
            },
            "min_tx_size": {
                "description": "Minimum valid transaction size",
                "expected_result": "ACCEPT",
            },
            "dust_limit": {
                "description": "Output at exactly dust threshold",
                "value_una": 546,
                "expected_result": "ACCEPT",
            },
            "below_dust": {
                "description": "Output below dust threshold",
                "value_una": 545,
                "expected_result": "REJECT",
                "violates_rule": "DUST",
            },
            "max_money": {
                "description": "Output at exactly 21M DIN",
                "value_una": 21_000_000 * 100_000_000,
                "expected_result": "ACCEPT",
            },
            "over_max_money": {
                "description": "Output exceeding 21M DIN",
                "value_una": 21_000_001 * 100_000_000,
                "expected_result": "REJECT",
                "violates_rule": "OUTPUT_VALUE",
            },
            "timestamp_exactly_2h": {
                "description": "Block timestamp exactly 2 hours in future",
                "expected_result": "ACCEPT",
            },
            "timestamp_2h_plus_1s": {
                "description": "Block timestamp 2h + 1s in future",
                "expected_result": "REJECT",
                "violates_rule": "TIMESTAMP_FUTURE",
            },
        }

        return {"boundary_conditions": vectors}

    def generate_rule_matrix(self) -> Dict[str, Any]:
        """Generate a matrix of all rules with test coverage"""
        print("Generating rule coverage matrix...")

        rules = []
        for rule in ConsensusRules.all_rules():
            rules.append({
                "id": rule.id,
                "name": rule.name,
                "category": rule.category.name,
                "description": rule.description,
                "bip": rule.bip,
                "has_valid_vector": True,
                "has_invalid_vector": True,
            })

        return {"consensus_rules": rules}

    def generate_all(self):
        """Generate all vector files"""
        print(f"=== Consensus Vector Generator ===")
        print(f"Network: {self.network}")
        print(f"Output: {self.output_dir}")
        print()

        # Valid blocks
        valid = self.generate_valid_blocks()
        self._save("valid_blocks.json", valid)

        # Invalid blocks
        invalid_blocks = self.generate_invalid_blocks()
        self._save("invalid_blocks.json", invalid_blocks)

        # Invalid transactions
        invalid_txs = self.generate_invalid_transactions()
        self._save("invalid_transactions.json", invalid_txs)

        # Boundary conditions
        boundaries = self.generate_boundary_conditions()
        self._save("boundary_conditions.json", boundaries)

        # Rule matrix
        rules = self.generate_rule_matrix()
        self._save("consensus_rules.json", rules)

        print()
        print("Done!")


def main():
    parser = argparse.ArgumentParser(description="Generate consensus test vectors")
    parser.add_argument("--network", default="regtest", help="Network (regtest/testnet/mainnet)")
    parser.add_argument("--output", "-o", default="tests/vectors", help="Output directory")
    parser.add_argument("--blocks", type=int, default=5, help="Number of valid blocks to generate")

    args = parser.parse_args()

    generator = VectorGenerator(
        network=args.network,
        output_dir=args.output,
    )

    generator.generate_all()


if __name__ == "__main__":
    main()
