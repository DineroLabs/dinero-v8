# Separation gates

Gate probes for the wallet/node separation (spec section 8). All `sep_*` targets
are `EXCLUDE_FROM_ALL`; each ctest builds its target first. Run with
`ctest --no-tests=error -R '^<name>$'`.

## W6: standalone wallet-FFI consumer (`SepWalletFfiStandalone`)

Question: do the standalone wallet FFI and the Rust bridge's static library set
link with no node libraries today?

Target `sep_wallet_ffi_standalone` (`wallet_ffi_standalone.cpp`, calls
`dinero_wallet_init` on a temp dir, nonzero on failure) is linked exactly as
`mobile-tauri/src-tauri/build.rs:29-38` links, with the archive files passed
directly (no `target_link_libraries`, so no transitive INTERFACE deps):

    -Wl,-force_load,libdinero_wallet_ffi.a   (GNU: --whole-archive ... --no-whole-archive; MSVC: /WHOLEARCHIVE)
    libdinero_wallet.a libdinero_crypto.a libdinero_consensus.a
    libsecp256k1.a libjsoncpp.a libsqlite3.a  -lc++  -framework Security -framework Foundation

build.rs lists secp256k1, jsoncpp, sqlite3, c++ and the system frameworks (dl,
pthread on Linux) in addition to the four archives named in the task brief; the
target follows build.rs. build.rs does not list OpenSSL, zstd, or any other
archive, and neither does this link.

### Baseline result (2026-10-29, macOS arm64, Release)

RED in both configurations (Orchard OFF and ON give an identical undefined-symbol
set): the link fails with 142 distinct undefined symbols. Per the owner rule the
ctest is labelled `separation;recorded` (not a required gate); the repair is E1.
Undefined symbols, grouped, with the archive that defines each, are in
`dinero-evidence/2026-10-29-separation-stage0/w6-{off,on}.log`:

- node/chainstate (`ChainDB`, `BlockStorage`): 5 (libdinero_chainstate.a)
- mempool (`Mempool::isOutputSpentInMempool`): 1 (nodecore_ffi / test stub)
- RocksDB: 0
- sqlite-owner stores (`ShieldedNoteStore`, `V7P2MRStore`, `OrchardAccountCatalog`,
  `UTXOIndex`, `open_sqlite`): 39 (libdinero_wallet_catalog.a, libwallet_test_deps.a, ...)
- OpenSSL (`EVP_*`, `HMAC`, `RAND_bytes`, ...): 33, since build.rs does not link OpenSSL
- zstd: 4
- other (libdinero_shielded, libdinero_pq, libdinero_bip32, libdinero_address_encoding,
  argon2): 60

When the result turns GREEN, flip the label in `CMakeLists.txt` to
`separation;required` and update this note to "W6 required (baseline green)".
