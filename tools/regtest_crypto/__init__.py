"""
DineroCoin Regtest Crypto Module

Reference implementation for cryptographic operations.
Used for test vector generation, NOT runtime dependencies.

Usage:
    from tools.regtest_crypto import pow, merkle, tx, keys

    # Mine a header
    nonce, hash_hex = pow.mine_header(header_bytes, bits=0x207fffff)

    # Compute merkle root
    root = merkle.merkle_root([txid1, txid2])

    # Sign a transaction
    sig = keys.sign_schnorr(privkey, sighash)
"""

from . import params
from . import pow
from . import merkle
from . import tx
from . import keys
from . import script
from . import vectors

__version__ = "1.0.0"
__all__ = ["params", "pow", "merkle", "tx", "keys", "script", "vectors"]
