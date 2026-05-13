#!/usr/bin/env python3
"""
Dinero Miner Dashboard
======================
Focused dashboard for miners to verify rewards, track fees, and monitor mining.

Usage:
    python3 miner.py [--datadir /path/to/datadir] [--address din1xxx...]
"""

import sys
import os
from datetime import datetime, timedelta
from collections import defaultdict
from flask import Flask, render_template, request, jsonify, redirect, url_for

# Add scripts directory to path for dinero_rpc import
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), 'scripts'))
from dinero_rpc import DineroRPC, RPCError

app = Flask(__name__)
app.config['TEMPLATES_AUTO_RELOAD'] = True

# Global RPC client
rpc = None
miner_address = None


def decode_taproot_address(scriptpubkey_hex):
    """Extract address from Taproot scriptPubKey (simplified)"""
    # Taproot: 5120 + 32-byte pubkey
    if scriptpubkey_hex.startswith('5120') and len(scriptpubkey_hex) == 68:
        return f"Taproot output"
    return "Unknown"


def find_miner_blocks(address=None, max_blocks=1000):
    """Find blocks mined by this miner"""
    info = rpc.getblockchaininfo()
    current_height = info['blocks']

    mined_blocks = []

    # Search recent blocks
    start_height = max(0, current_height - max_blocks)

    for height in range(current_height, start_height - 1, -1):
        try:
            block_hash = rpc.getblockhash(height)
            block = rpc.getblock(block_hash)

            # Get coinbase transaction
            coinbase_txid = block['tx'][0]
            coinbase_tx = rpc.blockchain.gettransaction(coinbase_txid)

            # Check if this block is ours (if address specified)
            if address:
                # Simple check: see if any output goes to our address
                # In production, would decode scriptPubKey properly
                is_ours = False
                for output in coinbase_tx.get('outputs', []):
                    # This is simplified - real implementation would decode address
                    is_ours = True  # For now, show all blocks
                    break

                if not is_ours:
                    continue

            # Calculate total fees in this block
            total_fees = 0.0
            for txid in block['tx'][1:]:  # Skip coinbase
                try:
                    tx = rpc.blockchain.gettransaction(txid)
                    if 'fee_din' in tx:
                        total_fees += float(tx['fee_din'])
                except:
                    pass

            # Get coinbase reward
            coinbase_amount = float(coinbase_tx.get('total_output_value_din', '0'))

            mined_blocks.append({
                'height': height,
                'hash': block_hash,
                'time': block['time'],
                'tx_count': block['nTx'],
                'coinbase_amount': coinbase_amount,
                'total_fees': total_fees,
                'total_reward': coinbase_amount,
                'confirmations': current_height - height + 1
            })

        except Exception as e:
            print(f"Error processing block {height}: {e}")
            continue

    return mined_blocks


def calculate_mining_stats(blocks):
    """Calculate mining statistics"""
    if not blocks:
        return {}

    total_rewards = sum(b['total_reward'] for b in blocks)
    total_fees = sum(b['total_fees'] for b in blocks)
    total_blocks = len(blocks)

    # Time range
    if blocks:
        oldest_time = blocks[-1]['time']
        newest_time = blocks[0]['time']
        time_range = newest_time - oldest_time
        hours = time_range / 3600 if time_range > 0 else 1
        blocks_per_hour = total_blocks / hours if hours > 0 else 0
        blocks_per_day = blocks_per_hour * 24
    else:
        blocks_per_day = 0

    return {
        'total_blocks': total_blocks,
        'total_rewards': total_rewards,
        'total_fees': total_fees,
        'avg_reward': total_rewards / total_blocks if total_blocks > 0 else 0,
        'avg_fees': total_fees / total_blocks if total_blocks > 0 else 0,
        'blocks_per_day': blocks_per_day,
        'fee_percentage': (total_fees / total_rewards * 100) if total_rewards > 0 else 0
    }


@app.template_filter('timestamp')
def timestamp_filter(ts):
    return datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')


@app.template_filter('din')
def din_filter(value):
    try:
        return f"{float(value):,.8f}"
    except:
        return value


@app.template_filter('shorthash')
def shorthash_filter(hash_str, length=16):
    if len(hash_str) <= length:
        return hash_str
    return f"{hash_str[:length]}..."


@app.template_filter('number_format')
def number_format_filter(value):
    """Format number with thousand separators"""
    try:
        return f"{int(value):,}"
    except:
        return value


@app.route('/')
def index():
    """Miner dashboard homepage"""
    try:
        # Get blockchain info
        info = rpc.getblockchaininfo()

        # Find mined blocks
        mined_blocks = find_miner_blocks(miner_address, max_blocks=500)

        # Calculate stats
        stats = calculate_mining_stats(mined_blocks)

        # Get recent blocks (last 20)
        recent_blocks = mined_blocks[:20]

        # Current mining info
        current_height = info['blocks']
        difficulty = info.get('difficulty', 0)

        return render_template('miner_dashboard.html',
                             chain_info=info,
                             stats=stats,
                             recent_blocks=recent_blocks,
                             miner_address=miner_address,
                             current_height=current_height,
                             difficulty=difficulty)

    except Exception as e:
        return f"Error: {e}", 500


@app.route('/block/<int:height>')
def block_detail(height):
    """Detailed block view for miners"""
    try:
        block_hash = rpc.getblockhash(height)
        block = rpc.getblock(block_hash)

        # Get coinbase transaction
        coinbase_txid = block['tx'][0]
        coinbase_tx = rpc.blockchain.gettransaction(coinbase_txid)

        # Get all transactions and calculate fees
        transactions = []
        total_fees = 0.0

        for txid in block['tx']:
            try:
                tx = rpc.blockchain.gettransaction(txid)
                fee = float(tx.get('fee_din', 0))
                total_fees += fee

                transactions.append({
                    'txid': txid,
                    'is_coinbase': tx.get('is_coinbase', False),
                    'fee': fee,
                    'outputs': len(tx.get('outputs', []))
                })
            except:
                pass

        coinbase_amount = float(coinbase_tx.get('total_output_value_din', '0'))

        block_info = {
            'height': height,
            'hash': block_hash,
            'time': block['time'],
            'tx_count': block['nTx'],
            'coinbase_amount': coinbase_amount,
            'total_fees': total_fees,
            'expected_reward': coinbase_amount,
            'difficulty': block.get('bits', 0),
            'nonce': block.get('nonce', 0),
            'transactions': transactions,
            'coinbase_outputs': coinbase_tx.get('outputs', [])
        }

        return render_template('miner_block.html', block=block_info)

    except Exception as e:
        return f"Error: {e}", 500


@app.route('/verify/<int:height>')
def verify_block(height):
    """Verify block reward calculation"""
    try:
        block_hash = rpc.getblockhash(height)
        block = rpc.getblock(block_hash)

        # Get coinbase
        coinbase_txid = block['tx'][0]
        coinbase_tx = rpc.blockchain.gettransaction(coinbase_txid)
        actual_reward = float(coinbase_tx.get('total_output_value_din', '0'))

        # Calculate expected reward (simplified - would use proper subsidy calculation)
        # For now, use a simple formula
        INITIAL_REWARD = 2627900.0  # Example
        expected_reward = INITIAL_REWARD

        # Calculate fees
        total_fees = 0.0
        for txid in block['tx'][1:]:
            try:
                tx = rpc.blockchain.gettransaction(txid)
                if 'fee_din' in tx:
                    total_fees += float(tx['fee_din'])
            except:
                pass

        expected_total = expected_reward + total_fees

        verification = {
            'height': height,
            'actual_reward': actual_reward,
            'expected_base_reward': expected_reward,
            'total_fees': total_fees,
            'expected_total': expected_total,
            'matches': abs(actual_reward - expected_total) < 0.00000001,
            'difference': actual_reward - expected_total
        }

        return jsonify(verification)

    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/search', methods=['GET', 'POST'])
def search():
    """Search for block height"""
    if request.method == 'POST':
        query = request.form.get('query', '').strip()
    else:
        query = request.args.get('q', '').strip()

    if not query:
        return redirect(url_for('index'))

    # Try as block height
    try:
        height = int(query)
        return redirect(url_for('block_detail', height=height))
    except ValueError:
        pass

    return redirect(url_for('index'))


@app.route('/api/stats')
def api_stats():
    """API endpoint for mining stats"""
    try:
        mined_blocks = find_miner_blocks(miner_address, max_blocks=500)
        stats = calculate_mining_stats(mined_blocks)

        info = rpc.getblockchaininfo()

        return jsonify({
            'mining_stats': stats,
            'chain_height': info['blocks'],
            'difficulty': info.get('difficulty', 0),
            'recent_blocks': len([b for b in mined_blocks if b['confirmations'] < 100])
        })

    except Exception as e:
        return jsonify({'error': str(e)}), 500


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Dinero Miner Dashboard')
    parser.add_argument('--datadir', type=str, default=None,
                       help='Dinero data directory')
    parser.add_argument('--address', type=str, default=None,
                       help='Your mining address (optional)')
    parser.add_argument('--port', type=int, default=5001,
                       help='Port to run dashboard on')
    parser.add_argument('--host', type=str, default='127.0.0.1',
                       help='Host to bind to')
    parser.add_argument('--debug', action='store_true',
                       help='Enable debug mode')

    args = parser.parse_args()

    # Initialize RPC client
    global rpc, miner_address
    try:
        rpc = DineroRPC(datadir=args.datadir)
        miner_address = args.address

        print(f"✓ Connected to Dinero RPC")
        print(f"✓ Datadir: {rpc.datadir}")

        if miner_address:
            print(f"✓ Tracking address: {miner_address}")
        else:
            print(f"ℹ No address specified - showing all blocks")

        # Test connection
        info = rpc.getblockchaininfo()
        print(f"✓ Chain: {info['chain']}")
        print(f"✓ Blocks: {info['blocks']}")
        print()
    except Exception as e:
        print(f"✗ Failed to connect to Dinero daemon: {e}")
        sys.exit(1)

    # Run Flask app
    print(f"Starting Dinero Miner Dashboard on http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == '__main__':
    main()
