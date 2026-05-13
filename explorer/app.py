#!/usr/bin/env python3
"""
Dinero Block Explorer
=====================
Minimal block explorer for Dinero blockchain using Flask and RPC client.

Usage:
    python3 app.py [--datadir /path/to/datadir] [--port 5000]
"""

import sys
import os
from datetime import datetime
from flask import Flask, render_template, request, redirect, url_for, jsonify

# Add scripts directory to path for dinero_rpc import
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(__file__)), 'scripts'))
from dinero_rpc import DineroRPC, RPCError

app = Flask(__name__)
app.config['TEMPLATES_AUTO_RELOAD'] = True

# Global RPC client (initialized in main)
rpc = None


def format_timestamp(timestamp):
    """Convert Unix timestamp to readable format"""
    return datetime.fromtimestamp(timestamp).strftime('%Y-%m-%d %H:%M:%S')


def format_din(value_str):
    """Format DIN amount with proper decimals"""
    try:
        value = float(value_str)
        return f"{value:,.8f}"
    except:
        return value_str


def shorten_hash(hash_str, length=16):
    """Shorten hash for display"""
    if len(hash_str) <= length:
        return hash_str
    return f"{hash_str[:length]}..."


@app.template_filter('timestamp')
def timestamp_filter(ts):
    return format_timestamp(ts)


@app.template_filter('din')
def din_filter(value):
    return format_din(value)


@app.template_filter('shorthash')
def shorthash_filter(hash_str):
    return shorten_hash(hash_str)


@app.template_filter('number_format')
def number_format_filter(value):
    """Format number with thousand separators"""
    try:
        return f"{int(value):,}"
    except:
        return value


@app.route('/')
def index():
    """Home page - Latest blocks"""
    try:
        # Get blockchain info
        info = rpc.getblockchaininfo()
        current_height = info['blocks']

        # Get last 20 blocks
        blocks = []
        for height in range(current_height, max(current_height - 20, -1), -1):
            try:
                block_hash = rpc.getblockhash(height)
                block = rpc.getblock(block_hash)

                blocks.append({
                    'height': height,
                    'hash': block['hash'],
                    'time': block['time'],
                    'tx_count': block['nTx'],
                    'confirmations': current_height - height + 1
                })
            except Exception as e:
                print(f"Error fetching block {height}: {e}")
                continue

        return render_template('index.html',
                             blocks=blocks,
                             chain_info=info)

    except Exception as e:
        return render_template('error.html',
                             error=f"Failed to fetch blockchain data: {e}")


@app.route('/block/<height_or_hash>')
def block_detail(height_or_hash):
    """Block detail page"""
    try:
        # Determine if input is height or hash
        try:
            height = int(height_or_hash)
            block_hash = rpc.getblockhash(height)
        except ValueError:
            # It's a hash
            block_hash = height_or_hash
            height = None

        # Get block data
        block = rpc.getblock(block_hash)

        # Get current height for confirmations
        current_height = rpc.getblockchaininfo()['blocks']

        # Get transaction details
        transactions = []
        total_fees = 0.0

        for txid in block['tx']:
            try:
                tx = rpc.blockchain.gettransaction(txid)

                # Calculate fee if available
                fee = 0.0
                if 'fee_din' in tx:
                    fee = float(tx['fee_din'])
                    total_fees += fee

                transactions.append({
                    'txid': txid,
                    'is_coinbase': tx.get('is_coinbase', False),
                    'input_count': tx.get('input_count', 0),
                    'output_count': tx.get('output_count', 0),
                    'total_output': tx.get('total_output_value_din', '0'),
                    'fee': fee
                })
            except Exception as e:
                print(f"Error fetching tx {txid}: {e}")
                transactions.append({
                    'txid': txid,
                    'error': str(e)
                })

        block_info = {
            'hash': block['hash'],
            'height': block['height'],
            'time': block['time'],
            'version': block['version'],
            'merkleroot': block['merkleroot'],
            'nonce': block['nonce'],
            'bits': block['bits'],
            'previousblockhash': block.get('previousblockhash', None),
            'confirmations': current_height - block['height'] + 1,
            'tx_count': block['nTx'],
            'total_fees': total_fees
        }

        return render_template('block.html',
                             block=block_info,
                             transactions=transactions)

    except RPCError as e:
        return render_template('error.html',
                             error=f"Block not found: {e}")
    except Exception as e:
        return render_template('error.html',
                             error=f"Error fetching block: {e}")


@app.route('/tx/<txid>')
def transaction_detail(txid):
    """Transaction detail page"""
    try:
        # Get transaction from blockchain
        tx = rpc.blockchain.gettransaction(txid)

        # Calculate totals
        total_input = 0.0
        total_output = 0.0

        if 'total_input_value_din' in tx:
            total_input = float(tx['total_input_value_din'])

        if 'total_output_value_din' in tx:
            total_output = float(tx['total_output_value_din'])

        fee = 0.0
        if 'fee_din' in tx:
            fee = float(tx['fee_din'])

        tx_info = {
            'txid': tx['txid'],
            'blockhash': tx.get('blockhash'),
            'blockheight': tx.get('blockheight'),
            'confirmations': tx.get('confirmations', 0),
            'time': tx.get('time'),
            'is_coinbase': tx.get('is_coinbase', False),
            'version': tx.get('version', 1),
            'locktime': tx.get('locktime', 0),
            'witness_version': tx.get('witness_version', 'legacy'),
            'status': tx.get('status', 'unknown'),
            'total_input': total_input,
            'total_output': total_output,
            'fee': fee,
            'inputs': tx.get('inputs', []),
            'outputs': tx.get('outputs', [])
        }

        return render_template('transaction.html', tx=tx_info)

    except RPCError as e:
        return render_template('error.html',
                             error=f"Transaction not found: {e}")
    except Exception as e:
        return render_template('error.html',
                             error=f"Error fetching transaction: {e}")


@app.route('/search', methods=['GET', 'POST'])
def search():
    """Search for block or transaction"""
    if request.method == 'POST':
        query = request.form.get('query', '').strip()
    else:
        query = request.args.get('q', '').strip()

    if not query:
        return redirect(url_for('index'))

    # Try as block height
    try:
        height = int(query)
        return redirect(url_for('block_detail', height_or_hash=height))
    except ValueError:
        pass

    # Try as block hash
    try:
        block = rpc.getblock(query)
        return redirect(url_for('block_detail', height_or_hash=query))
    except:
        pass

    # Try as transaction
    try:
        tx = rpc.blockchain.gettransaction(query)
        return redirect(url_for('transaction_detail', txid=query))
    except:
        pass

    return render_template('error.html',
                         error=f"Not found: {query}",
                         message="Could not find block or transaction with that identifier")


@app.route('/api/info')
def api_info():
    """API endpoint for blockchain info"""
    try:
        info = rpc.getblockchaininfo()
        return jsonify(info)
    except Exception as e:
        return jsonify({'error': str(e)}), 500


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Dinero Block Explorer')
    parser.add_argument('--datadir', type=str, default=None,
                       help='Dinero data directory')
    parser.add_argument('--port', type=int, default=5000,
                       help='Port to run explorer on')
    parser.add_argument('--host', type=str, default='127.0.0.1',
                       help='Host to bind to')
    parser.add_argument('--debug', action='store_true',
                       help='Enable debug mode')

    args = parser.parse_args()

    # Initialize RPC client
    global rpc
    try:
        rpc = DineroRPC(datadir=args.datadir)
        print(f"✓ Connected to Dinero RPC")
        print(f"✓ Datadir: {rpc.datadir}")

        # Test connection
        info = rpc.getblockchaininfo()
        print(f"✓ Chain: {info['chain']}")
        print(f"✓ Blocks: {info['blocks']}")
        print()
    except Exception as e:
        print(f"✗ Failed to connect to Dinero daemon: {e}")
        print(f"  Make sure dinerod is running")
        sys.exit(1)

    # Run Flask app
    print(f"Starting Dinero Block Explorer on http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=args.debug)


if __name__ == '__main__':
    main()
