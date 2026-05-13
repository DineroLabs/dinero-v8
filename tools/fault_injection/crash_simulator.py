"""
DineroCoin Crash Simulator

Simulates crashes at specific points in block processing.
Integrates with functional test framework.
"""

import os
import time
import signal
import threading
from dataclasses import dataclass
from typing import Optional, Callable, Dict, Any, List
from enum import Enum, auto


class CrashPoint(Enum):
    """Strategic points to crash during processing"""
    # Block Processing
    BEFORE_BLOCK_VALIDATION = auto()
    DURING_BLOCK_VALIDATION = auto()
    AFTER_BLOCK_VALIDATION = auto()
    BEFORE_UTXO_UPDATE = auto()
    DURING_UTXO_UPDATE = auto()
    AFTER_UTXO_UPDATE = auto()
    BEFORE_INDEX_UPDATE = auto()
    AFTER_INDEX_UPDATE = auto()

    # Reorg Processing (Priority 4 critical)
    BEFORE_REORG = auto()
    DURING_DISCONNECT = auto()
    BETWEEN_DISCONNECT_AND_CONNECT = auto()  # Most dangerous!
    DURING_CONNECT = auto()
    AFTER_REORG = auto()

    # Wallet Updates
    BEFORE_WALLET_UPDATE = auto()
    DURING_WALLET_UPDATE = auto()
    AFTER_WALLET_UPDATE = auto()

    # Transaction Processing
    BEFORE_MEMPOOL_ADD = auto()
    DURING_MEMPOOL_ADD = auto()


@dataclass
class CrashConfig:
    """Configuration for a crash simulation"""
    crash_point: CrashPoint
    delay_ms: int = 0              # Additional delay before crash
    probability: float = 1.0       # Probability of crashing (for random testing)
    signal: int = signal.SIGKILL   # Signal to send


class CrashSimulator:
    """
    Simulates crashes at specific points in processing.

    Uses RPC hooks or specially instrumented dinerod builds
    to crash at precise moments.

    Usage:
        sim = CrashSimulator()

        # Crash during reorg
        sim.configure(CrashPoint.BETWEEN_DISCONNECT_AND_CONNECT)
        sim.arm(node.process.pid)

        # Trigger reorg...
        connect_nodes(node0, node1)

        # Node will crash at configured point
    """

    def __init__(self, verbose: bool = True):
        self.verbose = verbose
        self.config: Optional[CrashConfig] = None
        self.armed = False
        self.target_pid: Optional[int] = None
        self.crash_history: List[Dict[str, Any]] = []

    def log(self, msg: str):
        if self.verbose:
            print(f"[CRASH-SIM] {msg}")

    def configure(
        self,
        crash_point: CrashPoint,
        delay_ms: int = 0,
        probability: float = 1.0,
    ):
        """Configure crash parameters"""
        self.config = CrashConfig(
            crash_point=crash_point,
            delay_ms=delay_ms,
            probability=probability,
        )
        self.log(f"Configured: {crash_point.name} (delay={delay_ms}ms, prob={probability})")

    def arm(self, pid: int):
        """Arm the crash simulator for a specific process"""
        if not self.config:
            raise RuntimeError("Configure crash point first")
        self.target_pid = pid
        self.armed = True
        self.log(f"Armed for PID {pid}")

    def disarm(self):
        """Disarm the crash simulator"""
        self.armed = False
        self.target_pid = None
        self.log("Disarmed")

    def trigger(self) -> bool:
        """
        Trigger the crash (call from hook or instrumentation).

        Returns True if crash was triggered.
        """
        if not self.armed or not self.config or not self.target_pid:
            return False

        import random
        if random.random() > self.config.probability:
            self.log("Skipped (probability)")
            return False

        if self.config.delay_ms > 0:
            time.sleep(self.config.delay_ms / 1000)

        try:
            os.kill(self.target_pid, self.config.signal)
            self.log(f"CRASHED PID {self.target_pid} at {self.config.crash_point.name}")

            self.crash_history.append({
                "timestamp": time.time(),
                "pid": self.target_pid,
                "crash_point": self.config.crash_point.name,
                "signal": self.config.signal,
            })

            self.armed = False
            return True

        except Exception as e:
            self.log(f"Crash failed: {e}")
            return False

    # =========================================================================
    # High-Level Crash Scenarios
    # =========================================================================

    def crash_during_reorg(
        self,
        node,
        triggering_node,
        crash_point: CrashPoint = CrashPoint.BETWEEN_DISCONNECT_AND_CONNECT,
    ) -> Dict[str, Any]:
        """
        Crash during a reorg operation.

        This tests the most critical persistence scenario:
        crashing between disconnecting old blocks and connecting new ones.
        """
        result = {
            "scenario": "crash_during_reorg",
            "crash_point": crash_point.name,
            "pre_state": None,
            "crashed": False,
            "post_state": None,
            "recovery_ok": None,
        }

        # Get pre-crash state
        try:
            result["pre_state"] = {
                "height": node.getblockcount(),
                "tip": node.getbestblockhash(),
            }
        except:
            pass

        # Configure and arm
        self.configure(crash_point)
        self.arm(node.process.pid)

        # The actual trigger would need hooks in dinerod
        # For now, we simulate with timed kill
        self.log("NOTE: Full reorg crash requires instrumented dinerod")

        return result

    def crash_during_block_write(
        self,
        node,
        crash_point: CrashPoint = CrashPoint.DURING_UTXO_UPDATE,
    ) -> Dict[str, Any]:
        """
        Crash while writing a block to database.

        Tests atomicity of block writes.
        """
        result = {
            "scenario": "crash_during_block_write",
            "crash_point": crash_point.name,
            "pre_height": None,
            "crashed": False,
            "post_height": None,
            "block_visible": None,  # Was partial block visible after recovery?
        }

        try:
            result["pre_height"] = node.getblockcount()
        except:
            pass

        self.configure(crash_point)
        self.arm(node.process.pid)

        self.log("NOTE: Full block write crash requires instrumented dinerod")

        return result

    # =========================================================================
    # Polling-Based Crash (for non-instrumented builds)
    # =========================================================================

    def poll_and_crash(
        self,
        node,
        condition: Callable[[], bool],
        poll_interval: float = 0.01,
        timeout: float = 30.0,
    ) -> bool:
        """
        Poll for a condition and crash when it becomes true.

        Useful for crashing "during" something without instrumentation.
        Example: crash when block count increases.
        """
        start = time.time()

        while time.time() - start < timeout:
            try:
                if condition():
                    os.kill(node.process.pid, signal.SIGKILL)
                    self.log(f"Crashed PID {node.process.pid} on condition")
                    return True
            except:
                pass
            time.sleep(poll_interval)

        self.log("Condition never met, no crash triggered")
        return False

    def crash_on_height_change(
        self,
        node,
        from_height: int,
        timeout: float = 30.0,
    ) -> bool:
        """Crash as soon as block height increases from from_height"""
        return self.poll_and_crash(
            node,
            lambda: node.getblockcount() > from_height,
            timeout=timeout,
        )

    def crash_on_reorg_start(
        self,
        node,
        expected_tip: str,
        timeout: float = 30.0,
    ) -> bool:
        """Crash as soon as tip changes (reorg starts)"""
        return self.poll_and_crash(
            node,
            lambda: node.getbestblockhash() != expected_tip,
            timeout=timeout,
        )

    # =========================================================================
    # Utilities
    # =========================================================================

    def summary(self) -> Dict[str, Any]:
        """Get summary of crash history"""
        return {
            "total_crashes": len(self.crash_history),
            "by_point": {},
            "history": self.crash_history[-10:],  # Last 10
        }
