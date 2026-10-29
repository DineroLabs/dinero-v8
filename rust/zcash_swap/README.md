# zcash_swap

Zcash v5 transparent transactions for DIN↔ZEC swaps, over librustzcash. The
design is `docs/design/din-zec-atomic-swaps.md`; this is its milestone 3.

The C ABI is `include/dinero_zcash_swap.h`:

| Function | What it does |
| --- | --- |
| `dzs_tx_build` | v5 transaction, transparent bundle only, explicit branch id, nLockTime and nExpiryHeight |
| `dzs_txid` | ZIP 244 txid of serialized bytes |
| `dzs_sighash_all` | ZIP 244 SIGHASH_ALL for one transparent input, given every spent output |
| `dzs_taddr_encode` / `dzs_taddr_decode` | t1 / t3 / tm / t2 addresses |
| `dzs_verify_input` | tests only (`script-verify` feature): one input through the zcash_script interpreter with zcashd's standard flags |

The crate holds no keys. `src/wallet/swap/zec_tx.cpp` builds the HTLC spends
and scriptSigs, and the wallet signs with libsecp256k1 in C++. The branch id
and expiry height are always arguments: the upgrade guard and the tower rung's
nExpiryHeight 0 are decisions the C++ engine makes.

## Build and test

```sh
cargo build --release --locked
```

The `SwapZecTx` ctest builds this crate with `--features script-verify` into
`<build>/zcash_swap-target` and runs `tests/wallet/test_swap_zec_tx.cpp`:

- the official ZIP 244 vectors, `tests/vectors/zip_0244.json`, from
  zcash/zcash-test-vectors at commit 113b3914c79dfe7eb68cb754cd7fea20b75e2e61
  (MIT or Apache-2.0; sha256 3d20892f19cec18afba2ef2907bb8426192d0ea4eac2c0060c2f31ff78ad93a3).
  These differ from the older set inside zcash_primitives 0.31.0, which
  upstream regenerated in September 2025; both pass;
- the HTLC claim, refund and tower rung, signed in C++ and run through the
  interpreter, including a wrong preimage, the wrong key, a refund before the
  lock and a refund with a final sequence;
- golden bytes for the signed spends.

`SwapZecRegtest` (when `zebrad` is found) runs `tests/wallet/swap_zec_regtest.sh`
against a fresh Zebra regtest node: the deposit (short, split, overpaid,
cancel), claim and refund as Zebra judges them, the watcher (decoy, mempool
reveal, reorg), the tower rung with nExpiryHeight 0, and a rung voided by a
network upgrade.

`SwapZecE2ERegtest` runs a whole DIN↔ZEC swap on dinerod + Zebra regtest
(tests/wallet/swap_zec_e2e_regtest.sh): happy, tower claims DIN for an offline
Bob, Bob silent, Alice silent (Bob refunds after T_zec), and the tower's rung
refunding an offline Bob. Zebra's blocks are stamped with the wall clock
(tests/wallet/zebra_regtest_mine.py); for the refunds zebrad runs under
libfaketime so its clock can pass T_zec (without it those steps are skipped).

`python3 rust/zcash_swap/tests/mutate.py <build-dir> --zebrad <path>` breaks 60
rules one at a time and checks that each makes a test fail (4 of them only the
regtest can catch).

## Versions and the fold-in

All dependencies are pinned with `=`. zcash_primitives 0.31 / zcash_transparent
0.11 was released on 2026-10-21. It is the only line that shares orchard 0.16,
incrementalmerkletree 0.9 and zip32 0.3 with `rust/orchard_backend` on v8.2.0.

Two Rust static libraries in one binary risk duplicate std symbols, so until
v8.2.1 this crate links into the test only. At v8.2.1 it becomes an rlib
dependency of `orchard_backend`, which then ships one static library. Checked
on 2026-10-28: `orchard_backend` with these crates added builds against its
patched `vendor/orchard`, and no existing pin moves (22 crates are added).

The Rust secp256k1 crate is linked either way, through zcash_transparent. Its C
symbols carry a `rustsecp256k1_v0_14_` prefix, so they do not clash with
libsecp256k1.
