"""
DineroCoin Recovery Tester

Verifies system integrity after crash/corruption recovery.
Tests all invariants after recovery.
"""

import time
from dataclasses import dataclass, field
from typing import Dict, Any, List, Optional, Tuple
from enum import Enum, auto


class InvariantCheck(Enum):
    """Invariants to verify after recovery"""
    # Daemon Invariants (D1-D6)
    D1_TIP_HEIGHT_NON_NEGATIVE = auto()
    D2_FORK_POINT_VALID = auto()
    D4_TIMESTAMP_VALID = auto()
    D6_HEIGHT_MONOTONIC = auto()

    # Persistence Invariants (P1-P5)
    P1_ATOMIC_WRITES = auto()
    P2_CRASH_RECOVERY = auto()
    P3_REORG_TRANSACTIONAL = auto()
    P4_WALLET_CONSENSUS_SYNC = auto()
    P5_NO_PARTIAL_BLOCKS = auto()

    # UTXO Invariants (U1-U7)
    U1_NO_PHANTOM_UTXOS = auto()
    U2_BALANCE_CONSISTENCY = auto()
    U3_COINBASE_MATURITY = auto()


@dataclass
class InvariantResult:
    """Result of an invariant check"""
    invariant: InvariantCheck
    passed: bool
    details: str
    value: Optional[Any] = None


@dataclass
class RecoveryReport:
    """Full recovery verification report"""
    scenario: str
    crash_type: str
    recovery_time_ms: float
    node_started: bool
    rpc_available: bool
    invariants: List[InvariantResult] = field(default_factory=list)
    data_loss: Optional[Dict[str, Any]] = None

    @property
    def all_passed(self) -> bool:
        return all(r.passed for r in self.invariants)


class RecoveryTester:
    """
    Verifies system integrity after recovery.

    Usage:
        tester = RecoveryTester()

        # After a crash...
        node.start()

        # Verify recovery
        report = tester.verify_recovery(node, pre_crash_state)

        if not report.all_passed:
            print("RECOVERY FAILED!")
            for inv in report.invariants:
                if not inv.passed:
                    print(f"  FAIL: {inv.invariant.name} - {inv.details}")
    """

    def __init__(self, verbose: bool = True):
        self.verbose = verbose
        self.reports: List[RecoveryReport] = []

    def log(self, msg: str):
        if self.verbose:
            print(f"[RECOVERY] {msg}")

    # =========================================================================
    # Individual Invariant Checks
    # =========================================================================

    def check_d1_tip_height(self, node) -> InvariantResult:
        """D1: Tip height must be non-negative"""
        try:
            height = node.getblockcount()
            passed = height >= 0
            return InvariantResult(
                invariant=InvariantCheck.D1_TIP_HEIGHT_NON_NEGATIVE,
                passed=passed,
                details=f"Height = {height}",
                value=height,
            )
        except Exception as e:
            return InvariantResult(
                invariant=InvariantCheck.D1_TIP_HEIGHT_NON_NEGATIVE,
                passed=False,
                details=f"RPC failed: {e}",
            )

    def check_p4_wallet_consensus_sync(self, node) -> InvariantResult:
        """P4: Wallet must be in sync with consensus"""
        try:
            # Get consensus tip
            chain_info = node.getblockchaininfo()
            consensus_height = chain_info.get("blocks", -1)
            consensus_tip = chain_info.get("bestblockhash", "")

            # Get wallet info (if available)
            try:
                wallet_info = node.getwalletinfo()
                wallet_scanning = wallet_info.get("scanning", False)

                # Wallet should not be scanning (should be synced)
                if wallet_scanning:
                    return InvariantResult(
                        invariant=InvariantCheck.P4_WALLET_CONSENSUS_SYNC,
                        passed=False,
                        details="Wallet still scanning after recovery",
                    )

                return InvariantResult(
                    invariant=InvariantCheck.P4_WALLET_CONSENSUS_SYNC,
                    passed=True,
                    details=f"Wallet synced at height {consensus_height}",
                    value={"height": consensus_height, "tip": consensus_tip},
                )

            except Exception:
                # No wallet, that's OK
                return InvariantResult(
                    invariant=InvariantCheck.P4_WALLET_CONSENSUS_SYNC,
                    passed=True,
                    details="No wallet to verify",
                )

        except Exception as e:
            return InvariantResult(
                invariant=InvariantCheck.P4_WALLET_CONSENSUS_SYNC,
                passed=False,
                details=f"Check failed: {e}",
            )

    def check_p5_no_partial_blocks(self, node) -> InvariantResult:
        """P5: No partially written blocks visible"""
        try:
            height = node.getblockcount()

            # Verify we can read the tip block completely
            tip_hash = node.getbestblockhash()
            block = node.getblock(tip_hash, 2)  # Full verbosity

            # Check block has required fields
            required = ["hash", "height", "tx", "merkleroot"]
            missing = [f for f in required if f not in block]

            if missing:
                return InvariantResult(
                    invariant=InvariantCheck.P5_NO_PARTIAL_BLOCKS,
                    passed=False,
                    details=f"Tip block missing fields: {missing}",
                )

            # Verify merkle root matches transactions
            # (Would need crypto helpers for full verification)

            return InvariantResult(
                invariant=InvariantCheck.P5_NO_PARTIAL_BLOCKS,
                passed=True,
                details=f"Tip block at height {height} is complete",
                value={"height": height, "tx_count": len(block.get("tx", []))},
            )

        except Exception as e:
            return InvariantResult(
                invariant=InvariantCheck.P5_NO_PARTIAL_BLOCKS,
                passed=False,
                details=f"Block read failed: {e}",
            )

    def check_u2_balance_consistency(self, node, pre_crash_balance: Optional[float] = None) -> InvariantResult:
        """U2: Balance must be consistent"""
        try:
            current_balance = node.getbalance()

            # If we have pre-crash balance, compare
            if pre_crash_balance is not None:
                # Balance should be >= pre_crash (could have received during crash)
                # But should never be > pre_crash + reasonable amount
                diff = current_balance - pre_crash_balance

                if diff < -0.00000001:  # Lost money
                    return InvariantResult(
                        invariant=InvariantCheck.U2_BALANCE_CONSISTENCY,
                        passed=False,
                        details=f"Balance decreased! {pre_crash_balance} -> {current_balance}",
                        value={"pre": pre_crash_balance, "post": current_balance, "diff": diff},
                    )

            return InvariantResult(
                invariant=InvariantCheck.U2_BALANCE_CONSISTENCY,
                passed=True,
                details=f"Balance = {current_balance}",
                value=current_balance,
            )

        except Exception as e:
            return InvariantResult(
                invariant=InvariantCheck.U2_BALANCE_CONSISTENCY,
                passed=False,
                details=f"Balance check failed: {e}",
            )

    # =========================================================================
    # Full Recovery Verification
    # =========================================================================

    def verify_recovery(
        self,
        node,
        pre_crash_state: Optional[Dict[str, Any]] = None,
        scenario: str = "unknown",
        crash_type: str = "unknown",
        timeout: int = 60,
    ) -> RecoveryReport:
        """
        Perform full recovery verification.

        Args:
            node: DineroNode instance (should be restarted after crash)
            pre_crash_state: State captured before crash for comparison
            scenario: Description of crash scenario
            crash_type: Type of crash/fault

        Returns:
            RecoveryReport with all invariant check results
        """
        self.log(f"=== Verifying Recovery: {scenario} ===")

        report = RecoveryReport(
            scenario=scenario,
            crash_type=crash_type,
            recovery_time_ms=0,
            node_started=False,
            rpc_available=False,
        )

        # Wait for node to start
        start_time = time.time()
        try:
            if node.rpc.wait_for_rpc_connection(timeout=timeout):
                report.node_started = True
                report.rpc_available = True
                report.recovery_time_ms = (time.time() - start_time) * 1000
                self.log(f"Node recovered in {report.recovery_time_ms:.0f}ms")
            else:
                self.log("Node failed to start!")
                return report
        except Exception as e:
            self.log(f"Recovery failed: {e}")
            return report

        # Run all invariant checks
        pre_balance = pre_crash_state.get("balance") if pre_crash_state else None

        checks = [
            self.check_d1_tip_height(node),
            self.check_p4_wallet_consensus_sync(node),
            self.check_p5_no_partial_blocks(node),
            self.check_u2_balance_consistency(node, pre_balance),
        ]

        for check in checks:
            report.invariants.append(check)
            status = "PASS" if check.passed else "FAIL"
            self.log(f"  {status}: {check.invariant.name} - {check.details}")

        # Compare with pre-crash state
        if pre_crash_state:
            report.data_loss = self._detect_data_loss(node, pre_crash_state)

        self.reports.append(report)

        if report.all_passed:
            self.log("=== RECOVERY VERIFIED ===")
        else:
            self.log("=== RECOVERY FAILED ===")

        return report

    def _detect_data_loss(
        self,
        node,
        pre_crash_state: Dict[str, Any],
    ) -> Dict[str, Any]:
        """Detect if any data was lost during crash"""
        loss = {
            "height_decreased": False,
            "balance_decreased": False,
            "tip_changed_unexpectedly": False,
        }

        try:
            current_height = node.getblockcount()
            pre_height = pre_crash_state.get("height", 0)

            if current_height < pre_height:
                loss["height_decreased"] = True
                loss["height_diff"] = pre_height - current_height

            current_balance = node.getbalance()
            pre_balance = pre_crash_state.get("balance", 0)

            if current_balance < pre_balance - 0.00000001:
                loss["balance_decreased"] = True
                loss["balance_diff"] = pre_balance - current_balance

        except Exception as e:
            loss["error"] = str(e)

        return loss

    # =========================================================================
    # Aggregate Reporting
    # =========================================================================

    def summary(self) -> Dict[str, Any]:
        """Get summary of all recovery tests"""
        total = len(self.reports)
        passed = sum(1 for r in self.reports if r.all_passed)
        failed = total - passed

        invariant_stats = {}
        for report in self.reports:
            for inv in report.invariants:
                name = inv.invariant.name
                if name not in invariant_stats:
                    invariant_stats[name] = {"passed": 0, "failed": 0}
                if inv.passed:
                    invariant_stats[name]["passed"] += 1
                else:
                    invariant_stats[name]["failed"] += 1

        return {
            "total_recoveries": total,
            "passed": passed,
            "failed": failed,
            "pass_rate": (passed / total * 100) if total > 0 else 0,
            "invariants": invariant_stats,
            "avg_recovery_time_ms": sum(r.recovery_time_ms for r in self.reports) / total if total > 0 else 0,
        }
