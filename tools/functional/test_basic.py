#!/usr/bin/env python3
"""
Basic DineroCoin Functional Test

Tests basic node functionality:
- Node starts and responds to RPC
- Block mining works
- Multi-node sync works
"""

import sys
import os

# Add framework to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from framework import (
    DineroTestFramework,
    assert_equal,
    assert_greater_than,
    sync_blocks,
)


class BasicTest(DineroTestFramework):
    """Test basic node operations"""

    def set_test_params(self):
        self.num_nodes = 2

    def run_test(self):
        self.log("Testing basic RPC connectivity...")

        # Test: Both nodes respond to RPC
        for i, node in enumerate(self.nodes):
            info = node.getblockchaininfo()
            self.log(f"Node {i}: chain={info.get('chain')}, blocks={info.get('blocks')}")
            assert_equal(info.get('chain'), 'regtest', f"Node {i} wrong chain")

        # Test: Initial block count (just genesis)
        height0 = self.nodes[0].getblockcount()
        height1 = self.nodes[1].getblockcount()
        self.log(f"Initial heights: node0={height0}, node1={height1}")

        # Test: Mine blocks on node 0
        self.log("Mining 10 blocks on node 0...")
        address = self.nodes[0].getnewaddress()
        block_hashes = self.nodes[0].generatetoaddress(10, address)
        assert_equal(len(block_hashes), 10, "Should mine 10 blocks")

        new_height = self.nodes[0].getblockcount()
        self.log(f"Node 0 height after mining: {new_height}")
        assert_equal(new_height, height0 + 10, "Height should increase by 10")

        # Test: Blocks sync to node 1
        self.log("Waiting for blocks to sync to node 1...")
        self.sync_blocks(timeout=30)

        height1_after = self.nodes[1].getblockcount()
        self.log(f"Node 1 height after sync: {height1_after}")
        assert_equal(height1_after, new_height, "Node 1 should sync to same height")

        # Test: Both nodes have same best block
        hash0 = self.nodes[0].getbestblockhash()
        hash1 = self.nodes[1].getbestblockhash()
        assert_equal(hash0, hash1, "Both nodes should have same tip")
        self.log(f"Both nodes at tip: {hash0[:16]}...")

        # Test: Get block details
        block = self.nodes[0].getblock(hash0, 1)
        assert_equal(block['height'], new_height)
        self.log(f"Tip block has {len(block.get('tx', []))} transaction(s)")

        self.log("All basic tests passed!")


if __name__ == '__main__':
    BasicTest().main()
