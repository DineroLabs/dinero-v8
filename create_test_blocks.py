#!/usr/bin/env python3
"""
Create test blk00000.dat file with blocks 0-101 for reindex testing
Tests: multiple blocks, chain progression, coinbase maturity
"""
import struct
import hashlib
import os

# Regtest network magic bytes (little-endian)
REGTEST_MAGIC = 0xDAB5BFFA

def sha256d(data):
    """Double SHA256 hash"""
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()

def serialize_varint(n):
    """Serialize variable-length integer"""
    if n < 0xfd:
        return struct.pack('<B', n)
    elif n <= 0xffff:
        return struct.pack('<BH', 0xfd, n)
    elif n <= 0xffffffff:
        return struct.pack('<BI', 0xfe, n)
    else:
        return struct.pack('<BQ', 0xff, n)

def create_coinbase_tx(height, reward=50 * 100_000_000):
    """Create a coinbase transaction for the given height"""
    tx_data = struct.pack('<i', 2)  # TX version

    # Inputs (1 coinbase input)
    tx_data += serialize_varint(1)
    tx_data += b'\x00' * 32  # Null prevout hash
    tx_data += struct.pack('<I', 0xffffffff)  # Null prevout index

    # ScriptSig (coinbase data - include height for uniqueness)
    coinbase_script = struct.pack('<I', height) + b'\x01\x04'
    tx_data += serialize_varint(len(coinbase_script))
    tx_data += coinbase_script
    tx_data += struct.pack('<I', 0xffffffff)  # Sequence

    # Outputs (1 output - reward to miner)
    tx_data += serialize_varint(1)
    tx_data += struct.pack('<Q', reward)

    # ScriptPubKey (OP_TRUE - anyone-can-spend for consensus testing)
    pubkey_script = b'\x51'  # OP_TRUE
    tx_data += serialize_varint(len(pubkey_script))
    tx_data += pubkey_script

    # Locktime
    tx_data += struct.pack('<I', 0)

    return tx_data

def create_spending_tx(prev_txid_bytes, prev_vout, input_amount, output_amount):
    """
    Create a spending transaction that consumes a UTXO
    Args:
        prev_txid_bytes: Previous transaction ID (32 bytes, little-endian)
        prev_vout: Previous output index
        input_amount: Amount being spent (for fee calculation)
        output_amount: Amount to send to output
    Returns:
        Serialized transaction bytes
    """
    tx_data = struct.pack('<i', 2)  # TX version

    # Inputs (1 input - spending previous UTXO)
    tx_data += serialize_varint(1)
    tx_data += prev_txid_bytes  # Previous txid (32 bytes, already in correct format)
    tx_data += struct.pack('<I', prev_vout)  # Previous output index

    # ScriptSig (empty - spending OP_TRUE output requires no signature)
    tx_data += serialize_varint(0)  # Empty scriptSig
    tx_data += struct.pack('<I', 0xfffffffe)  # Sequence (RBF-enabled)

    # Outputs (1 output - send to new address)
    tx_data += serialize_varint(1)
    tx_data += struct.pack('<Q', output_amount)

    # ScriptPubKey (P2PKH to different address - all 0xFF instead of 0x00)
    pubkey_script = bytes.fromhex('76a914' + 'ff' * 20 + '88ac')
    tx_data += serialize_varint(len(pubkey_script))
    tx_data += pubkey_script

    # Locktime
    tx_data += struct.pack('<I', 0)

    return tx_data

def create_block_header(prev_block_hash_bytes, merkle_root_hash, timestamp):
    """
    Create a DineroCoin 128-byte block header (BlockHeader v1).

    Layout (matches include/mining/header_layout.h):
        0-3:     version (4 bytes, LE uint32)
        4-35:    prev_block_hash (32 bytes, internal order)
        36-67:   merkle_root (32 bytes, internal order)
        68-99:   utreexo_root (32 bytes, internal order)
        100-107: timestamp (8 bytes, LE uint64)
        108-111: difficulty (4 bytes, LE uint32)
        112-115: nonce (4 bytes, LE uint32)
        116-127: reserved (12 bytes, must be zeros)

    Args:
        prev_block_hash_bytes: Previous block hash (32 bytes, internal order)
        merkle_root_hash: Merkle root as bytes (32 bytes, internal order)
        timestamp: Unix timestamp (uint64)
    Returns:
        Serialized 128-byte header
    """
    header = struct.pack('<I', 1)            # version (4 bytes)
    header += prev_block_hash_bytes          # prev_block_hash (32 bytes)
    header += merkle_root_hash               # merkle_root (32 bytes)
    header += b'\x00' * 32                   # utreexo_root (32 bytes)
    header += struct.pack('<Q', timestamp)   # timestamp (8 bytes, uint64)
    header += struct.pack('<I', 0x207fffff)  # difficulty (4 bytes, regtest)
    header += struct.pack('<I', 0)           # nonce (4 bytes)
    header += b'\x00' * 12                   # reserved (12 bytes)

    assert len(header) == 128, f"Header must be 128 bytes, got {len(header)}"
    return header

def merkle_root(tx_hashes):
    """
    Compute merkle root from transaction hashes
    Handles any number of transactions (not just 2)
    """
    if len(tx_hashes) == 0:
        return b'\x00' * 32
    if len(tx_hashes) == 1:
        return tx_hashes[0]

    while len(tx_hashes) > 1:
        # If odd number of hashes, duplicate the last one
        if len(tx_hashes) % 2 == 1:
            tx_hashes.append(tx_hashes[-1])

        # Pair up hashes and hash each pair
        tx_hashes = [
            sha256d(tx_hashes[i] + tx_hashes[i+1])
            for i in range(0, len(tx_hashes), 2)
        ]

    return tx_hashes[0]

def create_block(height, prev_block_hash_bytes, timestamp, extra_txs=None):
    """
    Create a complete block with coinbase transaction and optional extra transactions.
    Args:
        height: Block height
        prev_block_hash_bytes: Previous block hash (32 bytes, internal order)
        timestamp: Unix timestamp
        extra_txs: Optional list of additional transaction bytes
    Returns:
        (block_data, block_hash_bytes, coinbase_txid)
    """
    # Create coinbase transaction
    coinbase_tx = create_coinbase_tx(height)
    coinbase_txid = sha256d(coinbase_tx)

    # Collect all transactions
    all_txs = [coinbase_tx]
    if extra_txs:
        all_txs.extend(extra_txs)

    # Calculate merkle root (handles any number of transactions)
    tx_hashes = [sha256d(tx) for tx in all_txs]
    merkle_root_hash = merkle_root(tx_hashes)

    # Create 128-byte header
    header = create_block_header(prev_block_hash_bytes, merkle_root_hash, timestamp)

    # Block hash = double SHA-256 of 128-byte header (internal byte order)
    block_hash = sha256d(header)

    # Assemble block (header + tx count + transactions)
    block_data = header
    block_data += serialize_varint(len(all_txs))
    for tx in all_txs:
        block_data += tx

    return block_data, block_hash, coinbase_txid

def create_blockchain(num_blocks):
    """
    Create a blockchain with num_blocks blocks (0 to num_blocks-1)
    Block 101 will include a spending transaction that spends block 1's coinbase
    Returns list of (block_data, block_hash_hex) tuples
    """
    blocks = []
    coinbase_txids = {}  # Track coinbase txids by height
    prev_hash = b'\x00' * 32  # Genesis prev hash (32 bytes)
    base_timestamp = 1704067200  # Jan 1, 2024

    for height in range(num_blocks):
        timestamp = base_timestamp + (height * 600)  # 10 min per block
        extra_txs = None

        # Special handling for block 101: add spending transaction
        if height == 101 and 1 in coinbase_txids:
            # Create spending transaction that consumes block 1's coinbase
            # Block 1 coinbase is now mature (100 blocks old)
            block1_coinbase_txid = coinbase_txids[1]

            input_amount = 50 * 100_000_000  # 50 DIN
            output_amount = 49 * 100_000_000  # 49 DIN (1 DIN fee)

            spending_tx = create_spending_tx(
                block1_coinbase_txid,  # Previous txid (32 bytes, internal order)
                0,  # Previous vout (first output)
                input_amount,
                output_amount
            )

            extra_txs = [spending_tx]
            print(f"  Block 101: Adding spending TX (spends block 1 coinbase)")
            print(f"             Input:  {block1_coinbase_txid[::-1].hex()[:16]}...:0 (50 DIN)")
            print(f"             Output: 49 DIN (1 DIN fee)")

        block_data, block_hash, coinbase_txid = create_block(height, prev_hash, timestamp, extra_txs)

        # Store coinbase txid for future spending
        coinbase_txids[height] = coinbase_txid

        block_hash_hex = block_hash[::-1].hex()  # Display format (reversed)
        blocks.append((block_data, block_hash_hex))

        # Update prev_hash for next block (internal byte order)
        prev_hash = block_hash

        # Progress indicator
        if (height + 1) % 10 == 0 or height == 0 or height == 101:
            tx_count = 2 if extra_txs else 1
            print(f"  Block {height:3d}: {block_hash_hex[:16]}... ({len(block_data)} bytes, {tx_count} tx)")

    return blocks

def write_block_file(output_path, blocks):
    """Write blocks to blk00000.dat file"""
    total_size = 0

    with open(output_path, 'wb') as f:
        for block_data, _ in blocks:
            # Write magic bytes
            f.write(struct.pack('<I', REGTEST_MAGIC))

            # Write block size
            f.write(struct.pack('<I', len(block_data)))

            # Write block data
            f.write(block_data)

            total_size += 8 + len(block_data)  # magic(4) + size(4) + data

    print(f"\n✅ Created {output_path}")
    print(f"   Blocks: {len(blocks)}")
    print(f"   Total size: {total_size:,} bytes")
    print(f"   Magic: 0x{REGTEST_MAGIC:08x}")

def setup_test_datadir(base_path, num_blocks=102):
    """Create test datadir with blockchain"""
    blocks_dir = os.path.join(base_path, 'blocks')
    chaindb_dir = os.path.join(base_path, 'blockchain', 'chaindb')

    os.makedirs(blocks_dir, exist_ok=True)
    os.makedirs(chaindb_dir, exist_ok=True)

    print(f"Generating {num_blocks} blocks (0-{num_blocks-1})...")
    blocks = create_blockchain(num_blocks)

    blk_path = os.path.join(blocks_dir, 'blk00000.dat')
    write_block_file(blk_path, blocks)

    print(f"\n✅ Test datadir created at: {base_path}")
    print(f"   Blocks: {blocks_dir}")
    print(f"   ChainDB: {chaindb_dir}")
    print(f"\nRun reindex with:")
    print(f"   ./build/bin/test_reindex {base_path}")
    print(f"\nExpected results:")
    print(f"   - Blocks processed: {num_blocks}")
    print(f"   - UTXOs created: {num_blocks} (one coinbase per block)")
    print(f"   - Chain tip: block {num_blocks - 1}")

if __name__ == '__main__':
    test_dir = './test_reindex_data'
    setup_test_datadir(test_dir, num_blocks=102)
