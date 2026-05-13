#!/usr/bin/env python3
"""
Test script demonstrating Python RPC client and input amount lookup
"""

import sys
sys.path.insert(0, '/Users/haydarevich/Documents/DineroCoin/scripts')

from dinero_rpc import DineroRPC

def main():
    print("=" * 70)
    print("Dinero RPC - Input Amount Lookup Demonstration")
    print("=" * 70)
    print()

    # Initialize RPC client
    rpc = DineroRPC(datadir="/tmp/dinero_data")
    print(f"✓ Connected to Dinero RPC at {rpc.host}:{rpc.port}")
    print(f"✓ Using datadir: {rpc.datadir}")
    print()

    # Get blockchain info
    info = rpc.getblockchaininfo()
    print(f"Chain: {info['chain']}")
    print(f"Blocks: {info['blocks']}")
    print()

    # Test 1: Coinbase transaction (block 1)
    print("Test 1: Coinbase Transaction (Block 1)")
    print("-" * 70)
    txid1 = "9322fc8a246b11bf65f2327aeda6106fedbce064bff0af59544564bb203d71c7"
    tx1 = rpc.blockchain.gettransaction(txid1)

    print(f"TXID: {tx1['txid'][:16]}...")
    print(f"Block Height: {tx1['blockheight']}")
    print(f"Confirmations: {tx1['confirmations']}")
    print(f"Is Coinbase: {tx1['is_coinbase']}")
    print(f"Inputs: {tx1['input_count']}")
    print(f"Outputs: {tx1['output_count']}")
    print(f"Total Output Value: {tx1['total_output_value_din']} DIN")

    # For coinbase, no fee calculation (inputs don't have amounts)
    if tx1['is_coinbase']:
        print("Fee: N/A (coinbase transaction)")
    print()

    # Test 2: Non-coinbase transaction
    print("Test 2: Regular Transaction")
    print("-" * 70)
    # Find a transaction with inputs
    wallet_txs = rpc.wallet.listtransactions()
    if wallet_txs:
        test_txid = wallet_txs[0]['txid']
        tx2 = rpc.blockchain.gettransaction(test_txid)

        print(f"TXID: {tx2['txid'][:16]}...")
        print(f"Block Height: {tx2.get('blockheight', 'N/A')}")
        print(f"Is Coinbase: {tx2['is_coinbase']}")
        print(f"Inputs: {tx2['input_count']}")
        print(f"Outputs: {tx2['output_count']}")
        print(f"Total Output Value: {tx2['total_output_value_din']} DIN")

        # Show input details
        print("\nInput Details:")
        for i, inp in enumerate(tx2['inputs']):
            print(f"  Input #{i}:")
            print(f"    Prevout TXID: {inp['prevout_txid'][:16]}...")
            print(f"    Prevout vout: {inp['prevout_vout']}")

            # Check if amount was resolved
            if 'value_din' in inp:
                print(f"    Amount: {inp['value_din']} DIN")
            else:
                print(f"    Amount: Not found (prevout may not exist in chain)")

        # Show fee if available
        if 'fee_din' in tx2:
            print(f"\nFee: {tx2['fee_din']} DIN ({tx2['fee_una']} una)")
        else:
            print(f"\nFee: Not calculated (input amounts not resolved)")
    else:
        print("No wallet transactions found")
    print()

    # Test 3: wallet.gettransaction with amount enrichment
    if wallet_txs:
        print("Test 3: Wallet Transaction (Enhanced)")
        print("-" * 70)
        wallet_tx = rpc.wallet.gettransaction(test_txid)

        print(f"TXID: {wallet_tx['txid'][:16]}...")
        print(f"Category: {wallet_tx.get('category', 'N/A')}")
        print(f"Amount: {wallet_tx.get('amount', 'N/A')} DIN")
        print(f"Confirmations: {wallet_tx['confirmations']}")

        if 'inputs' in wallet_tx:
            print(f"\nInputs: {wallet_tx['input_count']}")
            for i, inp in enumerate(wallet_tx['inputs'][:3]):  # Show first 3
                print(f"  Input #{i}:")
                print(f"    TXID: {inp['txid'][:16]}...")
                if 'value' in inp:
                    print(f"    Value: {inp['value']} DIN")

        if 'outputs' in wallet_tx:
            print(f"\nOutputs: {wallet_tx['output_count']}")
            for i, out in enumerate(wallet_tx['outputs'][:3]):  # Show first 3
                print(f"  Output #{i}:")
                print(f"    Value: {out['value']} DIN")
                print(f"    Type: {out['type']}")

        if 'fee' in wallet_tx:
            print(f"\nFee: {wallet_tx['fee']} DIN")
    print()

    print("=" * 70)
    print("✓ All tests completed successfully!")
    print("=" * 70)

if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
