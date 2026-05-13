"""
DineroCoin Network Parameters

Matches consensus/chainparams.h exactly.
"""

from dataclasses import dataclass
from typing import Dict

# =============================================================================
# Constants (must match C++ exactly)
# =============================================================================

# Amount units
UNA_PER_DIN = 100_000_000  # 1 DIN = 100,000,000 una

# Header sizes
DINERO_HEADER_SIZE = 128   # DineroCoin: 128-byte headers
BITCOIN_HEADER_SIZE = 80   # Bitcoin: 80-byte headers

# Premine (Block 1)
PREMINE_AMOUNT_UNA = 262_790_000_000_000  # 2,627,900 DIN
PREMINE_HEIGHT = 1

# Coinbase maturity
COINBASE_MATURITY = 100  # Blocks before coinbase can be spent

# =============================================================================
# Network Parameters
# =============================================================================

@dataclass(frozen=True)
class NetworkParams:
    """Immutable network parameters."""
    name: str
    hrp: str                    # Bech32m human-readable part
    bits: int                   # Default difficulty bits
    require_standard: bool      # Require standard PoW targets
    magic: int                  # P2P message magic bytes
    default_port: int           # Default P2P port
    pubkey_prefix: int          # P2PKH address version
    script_prefix: int          # P2SH address version
    coinbase_maturity: int      # Blocks until coinbase spendable

NETWORKS: Dict[str, NetworkParams] = {
    "mainnet": NetworkParams(
        name="mainnet",
        hrp="din",
        bits=0x1d00ffff,        # Mainnet minimum difficulty
        require_standard=True,
        magic=0xD1A0C0DE,
        default_port=20999,
        pubkey_prefix=0x00,
        script_prefix=0x05,
        coinbase_maturity=100,
    ),
    "testnet": NetworkParams(
        name="testnet",
        hrp="tdin",
        bits=0x1d00ffff,        # Same as mainnet for testnet
        require_standard=True,
        magic=0xDAB5BFFA,
        default_port=21000,
        pubkey_prefix=0x6F,
        script_prefix=0xC4,
        coinbase_maturity=100,
    ),
    "regtest": NetworkParams(
        name="regtest",
        hrp="rdin",
        bits=0x207fffff,        # Easy difficulty for instant mining
        require_standard=False,  # Allow non-standard targets
        magic=0xFABFB5DA,
        default_port=21001,
        pubkey_prefix=0x6F,
        script_prefix=0xC4,
        coinbase_maturity=100,
    ),
}

def get_network(name: str = "regtest") -> NetworkParams:
    """Get network parameters by name."""
    if name not in NETWORKS:
        raise ValueError(f"Unknown network: {name}. Valid: {list(NETWORKS.keys())}")
    return NETWORKS[name]

# =============================================================================
# Genesis Block Parameters (must match chainparams_impl.cpp)
# =============================================================================

GENESIS_PARAMS = {
    "mainnet": {
        "version": 1,
        "timestamp": 1704067200,  # 2024-01-01 00:00:00 UTC
        "bits": 0x1d00ffff,
        "nonce": 0,  # TODO: Fill with actual mined nonce
        "merkle_root": "0" * 64,  # TODO: Fill with actual value
        "hash": "0" * 64,  # TODO: Fill with actual genesis hash
    },
    "regtest": {
        "version": 1,
        "timestamp": 1704067200,
        "bits": 0x207fffff,
        "nonce": 0,
        "merkle_root": "0" * 64,
        "hash": "0" * 64,
    },
}
