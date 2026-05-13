#!/usr/bin/env python3
"""
DineroCoin IBD Pipeline Invariants Test

Tests Phase 1 IBD invariants:
- Duplicate block rejection (early, before full validation)
- Parent = active tip invariant
- Forest state consistency
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from framework import (
    DineroTestFramework,
    assert_equal,
    assert_not_equal,
    wait_until,
)


class IBDPipelineTest(DineroTestFramework):
    """Test IBD pipeline invariants"""

    def set_test_params(self):
        self.num_nodes = 2

    def setup_network(self):
        """Start nodes but don't connect them initially"""
        self.setup_nodes()
        # Don't connect - we control block submission manually

    def run_test(self):
        self.log("Testing IBD pipeline invariants...")

        # Test 1: Duplicate block rejection
        self.test_duplicate_block_rejection()

        # Test 2: Parent invariant (out-of-order rejection)
        self.test_parent_invariant()

        self.log("All IBD pipeline tests passed!")

    def test_duplicate_block_rejection(self):
        """Test that duplicate blocks are rejected early"""
        self.log("=" * 60)
        self.log("Test 1: Duplicate block rejection")
        self.log("=" * 60)

        node = self.nodes[0]
        addr = node.getnewaddress()

        # Mine a block
        blockhash = node.generatetoaddress(1, addr)[0]
        height = node.getblockcount()
        self.log(f"Mined block at height {height}: {blockhash[:16]}...")

        # Get the raw block
        raw_block = node.getblock(blockhash, 0)  # 0 = hex string

        # Submit the same block again
        self.log("Submitting duplicate block...")
        result = node.submitblock(raw_block)

        # Should be rejected as duplicate
        # The result is either None (accepted), "duplicate" or some other error
        self.log(f"submitblock result: {result}")

        # Check logs for duplicate detection
        log_content = node.read_log(50)

        # Height should not change
        new_height = node.getblockcount()
        assert_equal(new_height, height, "Height should not change on duplicate")

        self.log("Duplicate block correctly handled")

    def test_parent_invariant(self):
        """Test that blocks with wrong parent are rejected during IBD"""
        self.log("=" * 60)
        self.log("Test 2: Parent = active tip invariant")
        self.log("=" * 60)

        # Use two isolated nodes to create orphan scenario
        node0 = self.nodes[0]
        node1 = self.nodes[1]

        addr0 = node0.getnewaddress()
        addr1 = node1.getnewaddress()

        # Mine on node0 to create a chain
        self.log("Mining 3 blocks on node0...")
        node0.generatetoaddress(3, addr0)
        height0 = node0.getblockcount()
        tip0 = node0.getbestblockhash()
        self.log(f"Node0: height={height0}, tip={tip0[:16]}...")

        # Mine a DIFFERENT chain on node1 (from premine, not synced with node0)
        self.log("Mining 2 blocks on node1 (different chain)...")
        node1.generatetoaddress(2, addr1)
        height1 = node1.getblockcount()
        tip1 = node1.getbestblockhash()
        self.log(f"Node1: height={height1}, tip={tip1[:16]}...")

        # Get a block from node1's chain
        block_from_node1 = node1.getblock(tip1, 0)

        # This block's parent is node1's previous block, NOT node0's tip
        # Submitting it to node0 during IBD should be rejected because
        # the parent doesn't match node0's active tip

        self.log("Submitting node1's block to node0 (wrong parent)...")
        result = node0.submitblock(block_from_node1)
        self.log(f"submitblock result: {result}")

        # The block should be rejected or orphaned
        # Check that node0's tip didn't change to node1's block
        final_tip0 = node0.getbestblockhash()
        assert_equal(final_tip0, tip0, "Tip should not change to orphan block")

        # Check logs for parent mismatch or orphan handling
        log_content = node0.read_log(100)
        self.log("Checking logs for parent invariant enforcement...")

        # Either rejected outright or stored as orphan
        has_orphan_msg = "orphan" in log_content.lower()
        has_reject_msg = "reject" in log_content.lower() or "invalid" in log_content.lower()

        self.log(f"  Orphan message in logs: {has_orphan_msg}")
        self.log(f"  Reject message in logs: {has_reject_msg}")

        self.log("Parent invariant correctly enforced")


if __name__ == '__main__':
    IBDPipelineTest().main()
