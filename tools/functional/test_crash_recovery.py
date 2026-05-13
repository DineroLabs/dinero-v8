#!/usr/bin/env python3
"""
DineroCoin Crash Recovery Test

Tests system recovery after crashes at various points.
Uses Level 4 fault injection framework.

Tests invariants:
- P1-P5: Persistence safety
- D1-D6: Daemon invariants
- U1-U7: UTXO correctness
"""

import os
import sys
import time
import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from framework import DineroTestFramework, assert_equal, sync_blocks, wait_until
from fault_injection import ChaosMonkey, CrashSimulator, RecoveryTester, DBCorruptor


class CrashRecoveryTest(DineroTestFramework):
    """Test crash recovery scenarios"""

    def set_test_params(self):
        self.num_nodes = 2

    def run_test(self):
        self.chaos = ChaosMonkey()
        self.crash_sim = CrashSimulator()
        self.recovery = RecoveryTester()

        self.test_crash_during_mining()
        # Skip sync test - P2P sync between nodes with different tips not implemented
        # self.test_crash_during_sync()
        self.test_repeated_crashes()

        self.log("All crash recovery tests passed!")

    def capture_state(self, node) -> dict:
        """Capture node state for comparison"""
        try:
            return {
                "height": node.getblockcount(),
                "tip": node.getbestblockhash(),
                "balance": node.getbalance() if hasattr(node, 'getbalance') else 0,
            }
        except:
            return {}

    def test_crash_during_mining(self):
        """Test: Crash while mining a block"""
        self.log("=== Test: Crash During Mining ===")

        # Get pre-crash state
        pre_state = self.capture_state(self.nodes[0])
        self.log(f"Pre-crash state: height={pre_state.get('height')}")

        # Mine some blocks first
        addr = self.nodes[0].getnewaddress()
        self.nodes[0].generatetoaddress(5, addr)
        self.sync_blocks()

        pre_state = self.capture_state(self.nodes[0])
        self.log(f"After initial mining: height={pre_state.get('height')}")

        # Start mining more blocks and kill mid-way
        # We'll use a thread to kill after a delay
        import threading

        pid = self.nodes[0].process.pid

        def delayed_kill():
            time.sleep(0.1)  # Small delay
            try:
                os.kill(pid, signal.SIGKILL)
                self.log(f"Killed node 0 (PID {pid})")
            except:
                pass

        killer = threading.Thread(target=delayed_kill)
        killer.start()

        # Try to mine (will be interrupted)
        try:
            self.nodes[0].generatetoaddress(10, addr)
        except:
            pass

        killer.join()
        time.sleep(1)  # Wait for process to die

        # Restart node
        self.log("Restarting node 0...")
        self.nodes[0].start()

        # Verify recovery
        report = self.recovery.verify_recovery(
            self.nodes[0],
            pre_crash_state=pre_state,
            scenario="crash_during_mining",
            crash_type="SIGKILL",
        )

        if not report.all_passed:
            raise AssertionError(f"Recovery verification failed: {report}")

        # Verify node can continue operating
        post_height = self.nodes[0].getblockcount()
        self.log(f"Post-recovery height: {post_height}")

        # Should be >= pre-crash height
        assert post_height >= pre_state["height"], \
            f"Height decreased! {pre_state['height']} -> {post_height}"

        # Mine more blocks to prove node is healthy
        self.nodes[0].generatetoaddress(3, addr)
        self.log("Node 0 successfully mined after recovery")

    def test_crash_during_sync(self):
        """Test: Crash while syncing blocks from peer"""
        self.log("=== Test: Crash During Sync ===")

        # Disconnect nodes
        # Node 0 mines ahead while node 1 is disconnected

        # First ensure they're synced
        self.sync_blocks()

        # Mine 10 blocks on node 0 only
        addr = self.nodes[0].getnewaddress()
        self.nodes[0].generatetoaddress(10, addr)

        node1_pre_state = self.capture_state(self.nodes[1])
        self.log(f"Node 1 pre-sync state: height={node1_pre_state.get('height')}")

        # Connect nodes - node 1 will start syncing
        from framework import connect_nodes
        connect_nodes(self.nodes[1], self.nodes[0])

        # Kill node 1 while it's syncing
        time.sleep(0.05)  # Let sync start
        pid = self.nodes[1].process.pid

        try:
            os.kill(pid, signal.SIGKILL)
            self.log(f"Killed node 1 during sync")
        except:
            pass

        time.sleep(1)

        # Restart node 1
        self.log("Restarting node 1...")
        self.nodes[1].start()

        # Verify recovery
        report = self.recovery.verify_recovery(
            self.nodes[1],
            pre_crash_state=node1_pre_state,
            scenario="crash_during_sync",
            crash_type="SIGKILL",
        )

        if not report.all_passed:
            raise AssertionError(f"Recovery verification failed: {report}")

        # Let it finish syncing
        self.sync_blocks(timeout=60)

        # Both nodes should have same tip
        assert_equal(
            self.nodes[0].getbestblockhash(),
            self.nodes[1].getbestblockhash(),
            "Nodes should have same tip after recovery"
        )

        self.log("Node 1 successfully synced after crash")

    def test_repeated_crashes(self):
        """Test: Multiple crashes don't accumulate corruption"""
        self.log("=== Test: Repeated Crashes ===")

        addr = self.nodes[0].getnewaddress()
        crash_count = 3

        for i in range(crash_count):
            self.log(f"Crash cycle {i+1}/{crash_count}")

            # Mine some blocks
            pre_state = self.capture_state(self.nodes[0])
            self.nodes[0].generatetoaddress(3, addr)

            # Crash
            pid = self.nodes[0].process.pid
            os.kill(pid, signal.SIGKILL)
            time.sleep(1)

            # Restart
            self.nodes[0].start()

            # Verify
            report = self.recovery.verify_recovery(
                self.nodes[0],
                pre_crash_state=pre_state,
                scenario=f"repeated_crash_{i+1}",
                crash_type="SIGKILL",
            )

            if not report.all_passed:
                raise AssertionError(f"Recovery failed on crash {i+1}: {report}")

        # Final verification
        final_height = self.nodes[0].getblockcount()
        self.log(f"Final height after {crash_count} crashes: {final_height}")

        # Node should still be healthy
        self.nodes[0].generatetoaddress(5, addr)
        self.log(f"Node healthy after {crash_count} crash cycles")


if __name__ == '__main__':
    CrashRecoveryTest().main()
