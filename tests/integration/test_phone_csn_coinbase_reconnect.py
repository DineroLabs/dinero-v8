#!/usr/bin/env python3
"""A real phone reconnects locally validated coinbase-only bodies without CSN sidecars.
No chain DB edits or fabricated proof/forest state; uses public RPC and restarts.
"""
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import Node


def main():
    os.environ.pop('DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE', None)
    node=Node(sys.argv[1],sys.argv[2])
    print(f'Evidence: {node.root}',flush=True)
    try:
        node.start()
        node.rpc('wallet.createhd',[node.wallet,12,'',node.password])
        node.rpc('wallet.unlock',[node.password,3600])
        address=node.rpc('wallet.getnewaddress',['taproot','phone reconnect'])['address']
        node.generate(5,address)
        fork=node.rpc('getbestblockhash')
        node.stop()
        os.environ['DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE']='1'
        flags=['--sync-profile=ios_utreexo']
        node.start(flags)
        node.rpc('wallet.open',[node.wallet])
        node.rpc('wallet.unlock',[node.password,3600])
        blocks=node.generate(3,address)
        expected_header=node.rpc('getblockheader',[blocks[-1]])
        # A closed/reopened store must reconnect the same body, not re-fetch
        # proofdata from a bridge. Offline mode stays enabled throughout.
        node.stop()
        node.start(flags)
        assert node.rpc('getbestblockhash')==blocks[-1]
        for restart_at_fork in (False,True):
            node.rpc('invalidateblock',[blocks[0]])
            assert node.rpc('getbestblockhash')==fork
            if restart_at_fork:
                node.stop()
                node.start(flags)
                assert node.rpc('getbestblockhash')==fork
            node.rpc('reconsiderblock',[blocks[0]])
            assert node.rpc('getbestblockhash')==blocks[-1], 'phone did not reconnect local coinbase bodies'
            assert node.rpc('getblockcount')==8
            assert node.rpc('getblockheader',[blocks[-1]])==expected_header
            print('PASS three-block reconnect; restart at fork =',restart_at_fork,flush=True)
        # Replayed blocks must themselves remain disconnectable/replayable.
        node.rpc('invalidateblock',[blocks[-1]])
        assert node.rpc('getbestblockhash')==blocks[-2]
        node.rpc('reconsiderblock',[blocks[-1]])
        assert node.rpc('getbestblockhash')==blocks[-1]
        node.stop()
        node.start(flags)
        assert node.rpc('getbestblockhash')==blocks[-1]
        assert node.rpc('getblockheader',[blocks[-1]])==expected_header
        node.rpc('wallet.open',[node.wallet])
        assert node.rpc('wallet.unlock',[node.password,3600])['success']
        print('PASS repeated replay, durable restart and wallet reopen',flush=True)
    finally:
        if node.process is not None and node.process.poll() is None:
            node.stop()

if __name__=='__main__':main()
