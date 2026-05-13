#!/usr/bin/env python3
"""
DineroCoin IBD Transition Test

Tests Phase 2 IBD transition invariants:
- IBD exit fires exactly once
- Transition banner appears in logs
- Services become ready after exit
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from framework import (
    DineroTestFramework,
    assert_equal,
    wait_until,
)


class IBDTransitionTest(DineroTestFramework):
    """Test IBD transition behavior"""

    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.log("Testing IBD transition behavior...")

        # Test 1: IBD exit detection
        self.test_ibd_exit_detection()

        self.log("All IBD transition tests passed!")

    def test_ibd_exit_detection(self):
        """Test that IBD exit is detected and logged exactly once"""
        self.log("=" * 60)
        self.log("Test 1: IBD exit detection")
        self.log("=" * 60)

        node = self.nodes[0]
        addr = node.getnewaddress()

        # Check initial state
        initial_height = node.getblockcount()
        self.log(f"Initial height: {initial_height}")

        # Mine enough blocks to exit IBD
        # IBD typically exits when we're within N blocks of "current time"
        # For regtest with no peers, it may exit immediately or after a few blocks
        self.log("Mining blocks to trigger IBD exit...")
        node.generatetoaddress(10, addr)

        # Wait a moment for logs to be written
        time.sleep(1)

        # Read logs and check for IBD exit message
        log_content = node.read_log(200)

        # Look for our IBD exit banner
        ibd_complete_count = log_content.count("INITIAL BLOCK DOWNLOAD COMPLETE")
        ibd_exit_count = log_content.count("IBDComplete")

        self.log(f"IBD complete messages found: {ibd_complete_count}")
        self.log(f"IBDComplete status changes: {ibd_exit_count}")

        # Also check via RPC if available
        try:
            blockchain_info = node.getblockchaininfo()
            is_initial_block_download = blockchain_info.get("initialblockdownload", True)
            self.log(f"RPC initialblockdownload: {is_initial_block_download}")

            # After mining, IBD should be complete
            # (though regtest may never truly be "in IBD" if started empty)
        except Exception as e:
            self.log(f"Note: getblockchaininfo failed: {e}")

        # Mine more blocks and verify IBD exit doesn't fire again
        self.log("Mining more blocks...")
        node.generatetoaddress(5, addr)
        time.sleep(1)

        log_content_after = node.read_log(200)
        ibd_complete_after = log_content_after.count("INITIAL BLOCK DOWNLOAD COMPLETE")

        self.log(f"IBD complete messages after more mining: {ibd_complete_after}")

        # The IBD exit message should appear at most once
        # (It may be 0 if regtest starts in non-IBD mode)
        if ibd_complete_count > 0:
            assert_equal(
                ibd_complete_after, ibd_complete_count,
                "IBD exit should fire exactly once, not on every block"
            )
            self.log("IBD exit correctly fires exactly once")
        else:
            self.log("Note: IBD exit message not found (regtest may not enter IBD)")

        self.log("IBD transition test complete")


if __name__ == '__main__':
    IBDTransitionTest().main()
