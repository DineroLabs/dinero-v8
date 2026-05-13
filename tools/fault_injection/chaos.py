"""
DineroCoin Chaos Monkey

Injects faults to test system resilience.
Tests recovery from crashes, corruption, and network failures.
"""

import os
import signal
import time
import random
import subprocess
import shutil
from enum import Enum, auto
from dataclasses import dataclass
from typing import Optional, Callable, List, Dict, Any
from pathlib import Path


class FaultType(Enum):
    """Types of faults that can be injected"""
    SIGKILL = auto()           # Hard kill (no cleanup)
    SIGTERM = auto()           # Graceful shutdown
    SIGSTOP = auto()           # Pause process
    DISK_FULL = auto()         # Simulate disk full
    CORRUPT_DB = auto()        # Corrupt database file
    CORRUPT_BLOCK = auto()     # Corrupt block data
    NETWORK_DROP = auto()      # Drop network connection
    SLOW_DISK = auto()         # Slow down disk I/O
    OOM = auto()               # Out of memory


@dataclass
class FaultResult:
    """Result of a fault injection"""
    fault_type: FaultType
    timestamp: float
    success: bool
    details: str
    recovery_time: Optional[float] = None
    data_loss: Optional[bool] = None


class ChaosMonkey:
    """
    Chaos engineering for DineroCoin nodes.

    Injects faults at strategic points to test:
    - Crash recovery
    - Data integrity after failure
    - Reorg handling during crashes
    - Wallet/consensus consistency

    Usage:
        chaos = ChaosMonkey()

        # Kill node mid-block
        chaos.kill_during_block_processing(node)

        # Kill during reorg
        chaos.kill_during_reorg(node)

        # Corrupt and recover
        chaos.corrupt_and_verify_recovery(node)
    """

    def __init__(self, verbose: bool = True):
        self.verbose = verbose
        self.fault_log: List[FaultResult] = []

    def log(self, msg: str):
        if self.verbose:
            print(f"[CHAOS] {msg}")

    def _record(self, result: FaultResult):
        self.fault_log.append(result)
        status = "OK" if result.success else "FAIL"
        self.log(f"{result.fault_type.name}: {status} - {result.details}")

    # =========================================================================
    # Process Faults
    # =========================================================================

    def kill_process(self, pid: int, sig: int = signal.SIGKILL) -> FaultResult:
        """
        Kill a process with specified signal.

        SIGKILL (9): Immediate termination, no cleanup
        SIGTERM (15): Graceful shutdown request
        """
        try:
            os.kill(pid, sig)
            time.sleep(0.5)  # Wait for process to die

            # Check if dead
            try:
                os.kill(pid, 0)
                alive = True
            except OSError:
                alive = False

            result = FaultResult(
                fault_type=FaultType.SIGKILL if sig == signal.SIGKILL else FaultType.SIGTERM,
                timestamp=time.time(),
                success=not alive,
                details=f"PID {pid} {'still alive' if alive else 'terminated'}",
            )
        except Exception as e:
            result = FaultResult(
                fault_type=FaultType.SIGKILL,
                timestamp=time.time(),
                success=False,
                details=f"Failed to kill PID {pid}: {e}",
            )

        self._record(result)
        return result

    def pause_process(self, pid: int, duration: float = 5.0) -> FaultResult:
        """
        Pause a process for a duration (SIGSTOP/SIGCONT).

        Useful for simulating slow I/O or network delays.
        """
        try:
            os.kill(pid, signal.SIGSTOP)
            self.log(f"Paused PID {pid} for {duration}s")
            time.sleep(duration)
            os.kill(pid, signal.SIGCONT)
            self.log(f"Resumed PID {pid}")

            result = FaultResult(
                fault_type=FaultType.SIGSTOP,
                timestamp=time.time(),
                success=True,
                details=f"Paused PID {pid} for {duration}s",
            )
        except Exception as e:
            result = FaultResult(
                fault_type=FaultType.SIGSTOP,
                timestamp=time.time(),
                success=False,
                details=f"Failed to pause PID {pid}: {e}",
            )

        self._record(result)
        return result

    # =========================================================================
    # Timed Kill Scenarios
    # =========================================================================

    def kill_after_delay(
        self,
        pid: int,
        delay: float,
        sig: int = signal.SIGKILL
    ) -> FaultResult:
        """Kill process after a delay"""
        self.log(f"Will kill PID {pid} in {delay}s...")
        time.sleep(delay)
        return self.kill_process(pid, sig)

    def kill_during_action(
        self,
        pid: int,
        action: Callable[[], None],
        delay_ratio: float = 0.5,
        estimated_duration: float = 1.0,
    ) -> FaultResult:
        """
        Kill process during an action.

        Starts the action, waits for delay_ratio * estimated_duration,
        then kills the process.
        """
        import threading

        killed = False
        kill_result = None

        def killer():
            nonlocal killed, kill_result
            delay = delay_ratio * estimated_duration
            time.sleep(delay)
            kill_result = self.kill_process(pid, signal.SIGKILL)
            killed = True

        # Start killer thread
        t = threading.Thread(target=killer)
        t.start()

        # Run action (will be interrupted by kill)
        try:
            action()
        except Exception as e:
            self.log(f"Action interrupted: {e}")

        t.join()

        return kill_result

    # =========================================================================
    # High-Level Scenarios
    # =========================================================================

    def scenario_kill_mid_block(
        self,
        node,  # DineroNode from functional tests
        block_count: int = 3,
        kill_at_block: int = 2,
    ) -> Dict[str, Any]:
        """
        Kill node while it's processing a block.

        Scenario:
        1. Start mining blocks
        2. Kill node mid-way through
        3. Restart node
        4. Verify consistency

        Returns dict with results and verification status.
        """
        self.log(f"=== Scenario: Kill Mid-Block ===")
        self.log(f"Mining {block_count} blocks, killing at block {kill_at_block}")

        results = {
            "scenario": "kill_mid_block",
            "block_count": block_count,
            "kill_at_block": kill_at_block,
            "pre_height": None,
            "kill_result": None,
            "post_height": None,
            "consistent": None,
        }

        # Get pre-crash state
        try:
            results["pre_height"] = node.getblockcount()
        except:
            pass

        # This would need integration with the functional framework
        # For now, return template
        self.log("NOTE: Full scenario requires integration with DineroTestFramework")

        return results

    def scenario_kill_during_reorg(
        self,
        nodes,  # List of DineroNode
        fork_length: int = 5,
    ) -> Dict[str, Any]:
        """
        Kill node during a reorg.

        Scenario:
        1. Create two chains on isolated nodes
        2. Connect nodes to trigger reorg
        3. Kill one node mid-reorg
        4. Restart and verify consistency

        Tests Priority 4 persistence safety fixes.
        """
        self.log(f"=== Scenario: Kill During Reorg ===")

        results = {
            "scenario": "kill_during_reorg",
            "fork_length": fork_length,
            "reorg_detected": None,
            "kill_result": None,
            "recovery_consistent": None,
        }

        self.log("NOTE: Full scenario requires integration with DineroTestFramework")

        return results

    def scenario_repeated_crashes(
        self,
        node,
        crash_count: int = 5,
        blocks_between_crashes: int = 3,
    ) -> Dict[str, Any]:
        """
        Repeatedly crash and recover node.

        Tests that no corruption accumulates over multiple crash cycles.
        """
        self.log(f"=== Scenario: Repeated Crashes ===")
        self.log(f"Will crash {crash_count} times, {blocks_between_crashes} blocks between")

        results = {
            "scenario": "repeated_crashes",
            "crash_count": crash_count,
            "crashes": [],
            "final_consistent": None,
        }

        self.log("NOTE: Full scenario requires integration with DineroTestFramework")

        return results

    # =========================================================================
    # Utilities
    # =========================================================================

    def summary(self) -> Dict[str, Any]:
        """Get summary of all faults injected"""
        total = len(self.fault_log)
        successful = sum(1 for r in self.fault_log if r.success)

        by_type = {}
        for r in self.fault_log:
            name = r.fault_type.name
            if name not in by_type:
                by_type[name] = {"total": 0, "success": 0}
            by_type[name]["total"] += 1
            if r.success:
                by_type[name]["success"] += 1

        return {
            "total_faults": total,
            "successful": successful,
            "failed": total - successful,
            "by_type": by_type,
        }
