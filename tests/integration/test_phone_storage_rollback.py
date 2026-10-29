#!/usr/bin/env python3
"""Real migration: retain until a phone connects a block; desktop keeps rollback.
Usage: CYCLE_DAEMON MIGRATOR. Isolated regtest, no installed wallet changes.
"""
import os
from pathlib import Path
import shutil
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import Node, RpcError


def main():
    os.environ.pop('DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE', None)
    base=Node(sys.argv[1],sys.argv[2])
    print(f'Evidence: {base.root}', flush=True)
    base.start()
    base.rpc('wallet.createhd',[base.wallet,12,'',base.password])
    base.rpc('wallet.unlock',[base.password,3600])
    address=base.rpc('wallet.getnewaddress',['taproot','rollback fixture'])['address']
    base.generate(5,address)
    tip=base.rpc('getbestblockhash')
    base.stop()
    os.environ['DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE']='1'
    for profile in ('mac_fullblock','ios_utreexo'):
        node=Node(sys.argv[1],sys.argv[2])
        shutil.rmtree(node.datadir)
        shutil.copytree(base.datadir,node.datadir)
        flags=['--sync-profile='+profile]
        node.start(flags)
        legacy=node.datadir/'blockchain'/'chaindb.legacy-v1'
        retiring=Path(str(legacy)+'.retiring')
        assert node.rpc('getbestblockhash')==tip
        assert legacy.is_dir(), 'removed before connecting a block'
        node.stop()
        node.start(flags)
        assert legacy.is_dir(), 'restart without connection removed rollback'
        try:
            node.rpc('submitblock',['00'])
        except RpcError:
            pass
        assert node.rpc('getbestblockhash')==tip
        assert legacy.is_dir(), 'rejected candidate removed rollback'
        node.rpc('wallet.open',[node.wallet])
        node.rpc('wallet.unlock',[node.password,3600])
        node.generate(1,address)
        next_tip=node.rpc('getbestblockhash')
        assert next_tip!=tip
        assert legacy.exists()==(profile=='mac_fullblock'), profile
        assert not retiring.exists()
        node.stop()
        node.start(flags)
        assert node.rpc('getbestblockhash')==next_tip, 'restart lost durable connection'
        assert legacy.exists()==(profile=='mac_fullblock')
        node.rpc('wallet.open',[node.wallet])
        assert node.rpc('wallet.unlock',[node.password,3600])['success']
        node.rpc('invalidateblock',[next_tip])
        assert node.rpc('getbestblockhash')==tip, 'reorg needs removed rollback copy'
        node.stop()
        node.start(flags)
        assert node.rpc('getbestblockhash')==tip, 'disconnect was not durable'
        assert legacy.exists()==(profile=='mac_fullblock')
        node.stop()
        print('PASS',profile,'migration, first durable connection and restart',flush=True)

    node=Node(sys.argv[1],sys.argv[2])
    shutil.rmtree(node.datadir)
    shutil.copytree(base.datadir,node.datadir)
    os.environ['DINERO_TEST_STORAGE_UPGRADE_CRASH_AFTER']='rollback-retired'
    node.start(['--sync-profile=ios_utreexo'])
    os.environ.pop('DINERO_TEST_STORAGE_UPGRADE_CRASH_AFTER')
    node.rpc('wallet.open',[node.wallet])
    node.rpc('wallet.unlock',[node.password,3600])
    try:
        node.generate(1,address)
    except Exception:
        pass # process exits after durable retirement, before RPC can reply
    assert node.process.wait(timeout=30)==17, 'retirement crash hook did not fire'
    node.process.stdin.close()
    node.process=None
    legacy=node.datadir/'blockchain'/'chaindb.legacy-v1'
    retiring=Path(str(legacy)+'.retiring')
    assert not legacy.exists() and retiring.is_dir()
    # Model an interrupted remove_all as well as the actual process death.
    (retiring/'CURRENT').unlink()
    node.start(['--sync-profile=ios_utreexo'])
    assert node.rpc('getblockcount')==6, 'connection was not durable before cleanup'
    assert retiring.exists(), 'cleanup ran at startup before next connection'
    node.rpc('wallet.open',[node.wallet])
    node.rpc('wallet.unlock',[node.password,3600])
    node.generate(1,address)
    assert not legacy.exists() and not retiring.exists()
    node.stop()
    print('PASS process death and partial deletion retry after durable phone connection',flush=True)

if __name__=='__main__': main()
