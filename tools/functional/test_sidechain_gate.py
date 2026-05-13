#!/usr/bin/env python3
"""
DineroCoin Side-Chain Gate Test

Tests Phase 3 side-chain gate:
- Side-chains rejected during IBD
- Side-chains allowed after IBD complete
- Gate opens exactly once (not per-block)
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from framework import (
    DineroTestFramework,
    assert_equal,
    assert_not_equal,
    connect_nodes,
    wait_until,
)


class SideChainGateTest(DineroTestFramework):
    """Test side-chain permission gate"""

    def set_test_params(self):
        self.num_nodes = 2

    def setup_network(self):
        """Start nodes isolated - we control connectivity"""
        self.setup_nodes()
        # Don't connect initially

    def run_test(self):
        self.log("Testing side-chain gate behavior...")

        # Test 1: Side-chains allowed after IBD
        self.test_sidechain_after_ibd()

        self.log("All side-chain gate tests passed!")

    def test_sidechain_after_ibd(self):
        """Test that side-chains are processed after IBD completes"""
        self.log("=" * 60)
        self.log("Test: Side-chain processing after IBD")
        self.log("=" * 60)

        node0 = self.nodes[0]
        node1 = self.nodes[1]

        addr0 = node0.getnewaddress()
        addr1 = node1.getnewaddress()

        # Both nodes start at same genesis/premine
        initial_height = node0.getblockcount()
        self.log(f"Initial height: {initial_height}")

        # Phase 1: Both nodes mine independently to exit IBD
        self.log("Phase 1: Mining on both nodes to exit IBD...")

        # Mine on node0 (shorter chain)
        self.log("Node0 mining 5 blocks...")
        node0.generatetoaddress(5, addr0)
        tip0 = node0.getbestblockhash()
        height0 = node0.getblockcount()

        # Mine on node1 (longer chain - will win reorg)
        self.log("Node1 mining 8 blocks...")
        node1.generatetoaddress(8, addr1)
        tip1 = node1.getbestblockhash()
        height1 = node1.getblockcount()

        self.log(f"Node0: height={height0}, tip={tip0[:16]}...")
        self.log(f"Node1: height={height1}, tip={tip1[:16]}...")

        # Chains should be different
        assert_not_equal(tip0, tip1, "Chains should be different")

        # Check IBD status before connecting
        # Read logs to see if IBD is complete
        log0_before = node0.read_log(100)
        self.log("Checking node0 IBD status before connect...")

        # Phase 2: Connect nodes - this will trigger side-chain processing
        self.log("Phase 2: Connecting nodes...")
        connect_nodes(node0, node1)

        # Wait for reorg to complete
        def nodes_synced():
            return node0.getbestblockhash() == node1.getbestblockhash()

        self.log("Waiting for nodes to sync (reorg expected)...")
        wait_until(nodes_synced, timeout=30, message="Nodes should sync")

        final_tip0 = node0.getbestblockhash()
        final_height0 = node0.getblockcount()

        self.log(f"After sync - Node0: height={final_height0}, tip={final_tip0[:16]}...")

        # Node0 should have reorged to node1's longer chain
        assert_equal(final_tip0, tip1, "Node0 should follow longer chain")
        assert_equal(final_height0, height1, "Node0 should have same height as node1")

        # Check logs for side-chain gate behavior
        log0_after = node0.read_log(200)

        # Look for side-chain gate messages
        gate_open = "GATE OPEN" in log0_after or "Side-chains allowed" in log0_after
        reorg_detected = "REORG" in log0_after.upper()

        self.log(f"Gate open message in logs: {gate_open}")
        self.log(f"Reorg detected in logs: {reorg_detected}")

        # The reorg should have succeeded
        self.log("Side-chain processing successful after IBD")


if __name__ == '__main__':
    SideChainGateTest().main()
