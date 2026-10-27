#!/usr/bin/env python3
"""Persistent handoff retention warns once, keeps the copy, and retries cleanup.
The post-startup marker is a retention/logging fixture, not an authenticated handoff.
"""
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import Node


def main():
    os.environ.pop('DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE',None)
    node=Node(sys.argv[1],sys.argv[2])
    print(f'Evidence: {node.root}',flush=True)
    try:
        node.start()
        node.rpc('wallet.createhd',[node.wallet,12,'',node.password])
        node.rpc('wallet.unlock',[node.password,3600])
        address=node.rpc('wallet.getnewaddress',['taproot','retained rollback'])['address']
        node.generate(5,address)
        node.stop()
        os.environ['DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE']='1'
        node.start(['--sync-profile=ios_utreexo'])
        node.rpc('wallet.open',[node.wallet])
        node.rpc('wallet.unlock',[node.password,3600])
        chain=node.datadir/'blockchain'
        legacy=chain/'chaindb.legacy-v1'
        assert legacy.is_dir()
        # Install after startup so this test exercises only cleanup's retention
        # fence; it does not pretend to supply a valid activation handoff.
        marker=chain/'.orchard-handoff-selected-v1.done'
        marker.write_text('persistent retention fixture\n')
        for _ in range(3):
            node.generate(1,address)
            assert (legacy/'CURRENT').is_file(), 'retained rollback was removed'
        log=node.root/f'process-{node.starts}.log'
        reason='rollback cleanup deferred: activation handoff retains source identity'
        count=log.read_text(errors='replace').count(reason)
        assert count==1, f'persistent handoff logged {count} warnings for three blocks'
        # A different reason must still be reported, rather than all future
        # maintenance errors being silenced by the first handoff warning.
        marker.unlink()
        recovery=chain/'chaindb.upgrade-state'
        recovery.write_text('copying\n')
        node.generate(1,address)
        assert legacy.is_dir()
        text=log.read_text(errors='replace')
        assert 'rollback cleanup deferred: unfinished operation at' in text
        recovery.unlink()
        node.generate(1,address)
        assert not legacy.exists(), 'cleanup stopped retrying after retention cleared'
        node.stop()
        print('PASS one warning, retained copy, changed-reason warning and later cleanup',flush=True)
    finally:
        if node.process is not None and node.process.poll() is None:node.stop()

if __name__=='__main__':main()
