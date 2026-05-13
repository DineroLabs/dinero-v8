"""
DineroCoin Consensus Rules

Defines consensus rules that can be tested (and violated for negative tests).
Each rule has an ID, description, and generator for valid/invalid cases.
"""

from enum import Enum, auto
from dataclasses import dataclass
from typing import List, Optional, Callable, Any


class RuleCategory(Enum):
    """Categories of consensus rules"""
    BLOCK_STRUCTURE = auto()    # Size, format, merkle
    BLOCK_HEADER = auto()       # PoW, timestamp, difficulty
    TRANSACTION = auto()        # Inputs, outputs, scripts
    SCRIPT = auto()             # Script execution
    UTXO = auto()               # UTXO set management
    COINBASE = auto()           # Coinbase specific rules
    SOFTFORK = auto()           # Activation rules


@dataclass
class ConsensusRule:
    """Definition of a consensus rule"""
    id: str
    name: str
    category: RuleCategory
    description: str
    bip: Optional[str] = None  # Related BIP if any


class ValidationError(Exception):
    """Raised when a consensus rule is violated"""
    def __init__(self, rule_id: str, message: str, details: Optional[dict] = None):
        self.rule_id = rule_id
        self.message = message
        self.details = details or {}
        super().__init__(f"[{rule_id}] {message}")


class ConsensusRules:
    """
    DineroCoin consensus rules registry.

    Defines all rules that blocks/transactions must follow.
    Used to generate both valid and intentionally-invalid test vectors.
    """

    # =========================================================================
    # Block Structure Rules
    # =========================================================================

    BLOCK_MAX_SIZE = ConsensusRule(
        id="BLOCK_SIZE",
        name="Block Size Limit",
        category=RuleCategory.BLOCK_STRUCTURE,
        description="Block serialized size must not exceed 4MB (4,000,000 bytes)",
    )

    BLOCK_MAX_WEIGHT = ConsensusRule(
        id="BLOCK_WEIGHT",
        name="Block Weight Limit",
        category=RuleCategory.BLOCK_STRUCTURE,
        description="Block weight must not exceed 4,000,000 WU",
        bip="BIP-141",
    )

    BLOCK_MERKLE_ROOT = ConsensusRule(
        id="MERKLE_ROOT",
        name="Merkle Root Valid",
        category=RuleCategory.BLOCK_STRUCTURE,
        description="Header merkle_root must match merkle root of transactions",
    )

    BLOCK_WITNESS_COMMITMENT = ConsensusRule(
        id="WITNESS_COMMITMENT",
        name="Witness Commitment",
        category=RuleCategory.BLOCK_STRUCTURE,
        description="SegWit witness commitment must be valid if present",
        bip="BIP-141",
    )

    BLOCK_TX_COUNT = ConsensusRule(
        id="TX_COUNT",
        name="Transaction Count",
        category=RuleCategory.BLOCK_STRUCTURE,
        description="Block must have at least one transaction (coinbase)",
    )

    # =========================================================================
    # Block Header Rules
    # =========================================================================

    HEADER_POW = ConsensusRule(
        id="POW_VALID",
        name="Proof of Work",
        category=RuleCategory.BLOCK_HEADER,
        description="Block hash must be <= target derived from bits",
    )

    HEADER_PREV_BLOCK = ConsensusRule(
        id="PREV_BLOCK",
        name="Previous Block Hash",
        category=RuleCategory.BLOCK_HEADER,
        description="prev_block_hash must match parent block's hash",
    )

    HEADER_TIMESTAMP_FUTURE = ConsensusRule(
        id="TIMESTAMP_FUTURE",
        name="Timestamp Not Too Far Future",
        category=RuleCategory.BLOCK_HEADER,
        description="Block timestamp must not be >2 hours in future",
    )

    HEADER_TIMESTAMP_MEDIAN = ConsensusRule(
        id="TIMESTAMP_MEDIAN",
        name="Timestamp Above Median",
        category=RuleCategory.BLOCK_HEADER,
        description="Block timestamp must be > median of last 11 blocks",
        bip="BIP-113",
    )

    HEADER_DIFFICULTY = ConsensusRule(
        id="DIFFICULTY",
        name="Difficulty Valid",
        category=RuleCategory.BLOCK_HEADER,
        description="Block bits must match expected difficulty adjustment",
    )

    HEADER_VERSION = ConsensusRule(
        id="VERSION",
        name="Block Version",
        category=RuleCategory.BLOCK_HEADER,
        description="Block version must be valid for current height",
    )

    # =========================================================================
    # Transaction Rules
    # =========================================================================

    TX_NOT_EMPTY = ConsensusRule(
        id="TX_NOT_EMPTY",
        name="Transaction Not Empty",
        category=RuleCategory.TRANSACTION,
        description="Transaction must have at least one input and one output",
    )

    TX_SIZE = ConsensusRule(
        id="TX_SIZE",
        name="Transaction Size",
        category=RuleCategory.TRANSACTION,
        description="Transaction size must be within limits",
    )

    TX_INPUTS_EXIST = ConsensusRule(
        id="INPUTS_EXIST",
        name="Inputs Exist",
        category=RuleCategory.TRANSACTION,
        description="All inputs must reference existing unspent outputs",
    )

    TX_NO_DUPLICATE_INPUTS = ConsensusRule(
        id="NO_DUP_INPUTS",
        name="No Duplicate Inputs",
        category=RuleCategory.TRANSACTION,
        description="Transaction must not spend same output twice",
    )

    TX_OUTPUT_VALUE = ConsensusRule(
        id="OUTPUT_VALUE",
        name="Output Value Range",
        category=RuleCategory.TRANSACTION,
        description="Each output value must be >= 0 and <= 21M DIN",
    )

    TX_OUTPUT_SUM = ConsensusRule(
        id="OUTPUT_SUM",
        name="Output Sum Valid",
        category=RuleCategory.TRANSACTION,
        description="Sum of outputs must not exceed sum of inputs (+ coinbase)",
    )

    TX_DUST = ConsensusRule(
        id="DUST",
        name="Dust Limit",
        category=RuleCategory.TRANSACTION,
        description="Outputs must meet minimum dust threshold (policy, not consensus)",
    )

    TX_FINAL = ConsensusRule(
        id="TX_FINAL",
        name="Transaction Final",
        category=RuleCategory.TRANSACTION,
        description="Transaction must be final (nLockTime/sequence)",
        bip="BIP-68",
    )

    # =========================================================================
    # Coinbase Rules
    # =========================================================================

    COINBASE_FIRST = ConsensusRule(
        id="COINBASE_FIRST",
        name="Coinbase First",
        category=RuleCategory.COINBASE,
        description="First transaction must be coinbase, others must not be",
    )

    COINBASE_INPUT = ConsensusRule(
        id="COINBASE_INPUT",
        name="Coinbase Input",
        category=RuleCategory.COINBASE,
        description="Coinbase must have exactly one input with null prevout",
    )

    COINBASE_SCRIPTSIG_SIZE = ConsensusRule(
        id="COINBASE_SCRIPTSIG",
        name="Coinbase ScriptSig Size",
        category=RuleCategory.COINBASE,
        description="Coinbase scriptSig must be 2-100 bytes",
    )

    COINBASE_HEIGHT = ConsensusRule(
        id="COINBASE_HEIGHT",
        name="Coinbase Height",
        category=RuleCategory.COINBASE,
        description="Coinbase must include block height (BIP-34)",
        bip="BIP-34",
    )

    COINBASE_SUBSIDY = ConsensusRule(
        id="COINBASE_SUBSIDY",
        name="Coinbase Subsidy",
        category=RuleCategory.COINBASE,
        description="Coinbase output must not exceed subsidy + fees",
    )

    COINBASE_MATURITY = ConsensusRule(
        id="COINBASE_MATURITY",
        name="Coinbase Maturity",
        category=RuleCategory.COINBASE,
        description="Coinbase outputs cannot be spent until 100 confirmations",
    )

    # =========================================================================
    # UTXO Rules
    # =========================================================================

    UTXO_NOT_SPENT = ConsensusRule(
        id="UTXO_UNSPENT",
        name="UTXO Not Already Spent",
        category=RuleCategory.UTXO,
        description="Input must reference an unspent output",
    )

    UTXO_EXISTS = ConsensusRule(
        id="UTXO_EXISTS",
        name="UTXO Exists",
        category=RuleCategory.UTXO,
        description="Referenced UTXO must exist in the UTXO set",
    )

    # =========================================================================
    # Script Rules
    # =========================================================================

    SCRIPT_VALID = ConsensusRule(
        id="SCRIPT_VALID",
        name="Script Validates",
        category=RuleCategory.SCRIPT,
        description="Input script must satisfy output script",
    )

    SCRIPT_SIZE = ConsensusRule(
        id="SCRIPT_SIZE",
        name="Script Size",
        category=RuleCategory.SCRIPT,
        description="Scripts must not exceed size limits",
    )

    SCRIPT_OPS = ConsensusRule(
        id="SCRIPT_OPS",
        name="Script Ops Count",
        category=RuleCategory.SCRIPT,
        description="Scripts must not exceed operation count limits",
    )

    # =========================================================================
    # Class Methods
    # =========================================================================

    @classmethod
    def all_rules(cls) -> List[ConsensusRule]:
        """Get all defined consensus rules"""
        rules = []
        for name in dir(cls):
            attr = getattr(cls, name)
            if isinstance(attr, ConsensusRule):
                rules.append(attr)
        return rules

    @classmethod
    def rules_by_category(cls, category: RuleCategory) -> List[ConsensusRule]:
        """Get rules filtered by category"""
        return [r for r in cls.all_rules() if r.category == category]

    @classmethod
    def get_rule(cls, rule_id: str) -> Optional[ConsensusRule]:
        """Get a rule by its ID"""
        for rule in cls.all_rules():
            if rule.id == rule_id:
                return rule
        return None
