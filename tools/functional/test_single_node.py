#!/usr/bin/env python3
"""
Single Node Test - Minimal test to verify framework works.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from framework import DineroTestFramework, assert_equal


class SingleNodeTest(DineroTestFramework):
    """Test single node operations"""

    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.log("Testing single node...")

        # Test: Node responds to RPC
        info = self.nodes[0].getblockchaininfo()
        self.log(f"Chain: {info.get('chain')}")
        self.log(f"Blocks: {info.get('blocks')}")
        assert_equal(info.get('chain'), 'regtest')

        # Test: Get block count
        height = self.nodes[0].getblockcount()
        self.log(f"Block count: {height}")

        # Test: Generate blocks
        self.log("Generating 5 blocks...")
        addr = self.nodes[0].getnewaddress()
        hashes = self.nodes[0].generatetoaddress(5, addr)
        self.log(f"Generated {len(hashes)} blocks")

        # Verify height increased
        new_height = self.nodes[0].getblockcount()
        self.log(f"New height: {new_height}")
        assert_equal(new_height, height + 5)

        self.log("Single node test PASSED!")


if __name__ == '__main__':
    SingleNodeTest().main()
