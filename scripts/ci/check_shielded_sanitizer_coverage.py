#!/usr/bin/env python3
"""Reject shielded sanitizer builds whose tested source files lack instrumentation."""
import argparse
import json
import shlex
from pathlib import Path

COMPACT_DAEMON_SOURCES = [
    'src/daemon/p2p_header_parser.cpp',
    'src/daemon/services/p2p_service.cpp',
    'src/daemon/services/mempool_service.cpp',
    'tests/daemon/test_p2p_header_parser.cpp',
    'tests/daemon/test_daemon_service_release.cpp',
    'src/daemon/main.cpp',
    'src/daemon/block_acceptor.cpp',
    'src/daemon/mempool.cpp',
    'src/daemon/services/chainstate_service.cpp',
    'src/consensus/block_validation.cpp',
    'src/consensus/utreexo_accumulator.cpp',
    'src/storage/chain_db.cpp',
    'src/primitives/transaction.cpp',
    'src/primitives/transaction_serializer.cpp',
    'src/p2p/structural_validator.cpp',
    'src/rpc/shielded_rpc_json.cpp',
    'src/wallet/wallet_manager.cpp',
    'src/wallet/shielded_wallet_ops.cpp',
    'src/wallet/shielded_wallet_runtime.cpp',
    'src/wallet/shielded_note_store.cpp',
    'src/wallet/shielded_derivation.cpp',
    'src/consensus/shielded/compact.cpp',
    'src/consensus/shielded/compact_spartan_codec.cpp',
    'src/consensus/shielded/shielded_validation.cpp',
    'src/consensus/shielded/shielded_serialization.cpp',
    'src/consensus/shielded/commitment_tree.cpp',
    'src/consensus/shielded/nullifier_set.cpp',
    'src/consensus/shielded/shielded_circuit.cpp',
    'src/consensus/shielded/binding_sig.cpp',
    'src/consensus/shielded/range_proof.cpp',
    'src/zk/zkvm/scalar.cpp',
    'src/zk/zkvm/r1cs_spartan.cpp',
    'tests/consensus/test_compact_regtest_vectors.cpp',
    'tests/consensus/test_shielded_resource_limits.cpp',
    'tests/consensus/test_compact_activation.cpp',
    'src/test/shielded_validation_tests.cpp',
    'tests/consensus/test_shielded_reindex_equivalence.cpp',
    'tests/integration/shielded_tx_builder.cpp',
]


def enabled_sanitizers(arguments):
    enabled = set()
    for argument in arguments:
        if argument.startswith('-fsanitize='):
            enabled.update(argument.split('=', 1)[1].split(','))
        elif argument.startswith('-fno-sanitize='):
            disabled = set(argument.split('=', 1)[1].split(','))
            if 'all' in disabled:
                enabled.clear()
            else:
                enabled.difference_update(disabled)
    return enabled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('sanitizers', help='Required comma-separated sanitizer flags')
    parser.add_argument('--fuzz', action='store_true')
    parser.add_argument('--compact-daemon', action='store_true',
                        help='Audit full compact daemon/wallet qualification sources')
    args = parser.parse_args()
    sources = [
        'src/consensus/shielded/compact_spartan_codec.cpp',
        'src/zk/zkvm/scalar.cpp',
        'src/zk/zkvm/r1cs_spartan.cpp',
        'src/consensus/shielded/shielded_serialization.cpp',
        'src/wallet/shielded_derivation.cpp',
    ]
    sources += (['fuzz/fuzz_compact_spartan.cpp', 'fuzz/fuzz_shielded_surfaces.cpp']
                if args.fuzz else ['tests/zk/test_compact_spartan.cpp',
                                      'tests/zk/test_spartan_soundness.cpp'])
    if args.compact_daemon:
        if args.fuzz:
            parser.error('--fuzz and --compact-daemon are separate qualification scopes')
        sources = COMPACT_DAEMON_SOURCES
    entries = json.loads((args.build_dir / 'compile_commands.json').read_text())
    required = set(args.sanitizers.split(','))
    failures, checked = [], []
    for source in sources:
        matches = [entry for entry in entries
                   if Path(entry['file']).as_posix().endswith('/' + source)]
        if not matches:
            failures.append(f'{source}: absent from compile commands')
        for entry in matches:
            arguments = entry.get('arguments') or shlex.split(entry['command'])
            enabled = enabled_sanitizers(arguments)
            checked.append({'source': source, 'enabled': sorted(enabled)})
            if not required <= enabled:
                failures.append(f'{source}: missing {sorted(required - enabled)}')
    print(json.dumps({'required': sorted(required), 'checked': checked,
                      'failures': failures}, indent=2))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
