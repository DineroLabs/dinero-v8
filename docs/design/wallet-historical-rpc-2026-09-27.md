# Historical raw transaction signing

The actual raw-signing handler carries the resolver's explicit key policy into
Taproot signing. It gathers supplied or indexed prevouts before taking a wallet
lease, checks the requested wallet name, and pins the existing unlock owner
through key reads and signatures. Requests that already carry every witness
retain their keyless behavior while locked. The new lease method accepts only that lease's
RecoverySeed token, thread, database and session. The ordinary unpinned lookup
still refuses while a recovery token is held. An authorized held operation can
finish without a second timeout transition. No wallet is unlocked implicitly.

Historical imports keep their exact script and have no invented HD path.
Canonical keys use recorded derivation or watch metadata. The old handler's
address-table-to-HD-path synthesis is removed. Key copies and the seed owner
are released before chain-dependent consensus validation. Existing witnesses
remain unchanged and `complete` still requires all prevouts and the existing
selected-chain consensus validator. A signature alone does not establish that
an input is unspent or that a transaction was admitted or broadcast.

TransactionBuilder's named typed-key entry point uses exact outpoint bindings
and stages the entire signed transaction. A later input refusal leaves every
witness in the returned failure result unsigned. Its result carries public
outpoint identifiers, not secret hex. The existing hex entry point is retained
for compatibility; other production send paths still require migration.

The component fixture invokes the actual raw RPC handler with real WalletService,
encrypted reopen, historical imports, modern imports and HD keys. It uses
synthetic prevouts and actual script verification. No chainstate is installed,
so `complete` must remain false. Deterministic SQL callbacks test owner refusal;
there is no concurrent exploit, crash or production-wallet reproduction.

This is not complete authenticated inventory, historical deletion or backup
recovery, all-caller session ownership, PSBT migration, live sendmany/shielding,
pending/reservation ownership, or whole-node release qualification. Existing
PQ store-opening and generic HD fallback behavior remain narrower than the new
historical signing path. Mainnet activation remains unset.

Local qualification: fresh genuine backend ON/OFF full daemon plus four declared
wallet targets built, and 13 selected CTests passed in each configuration. All
264 linked project C++ files were freshly rebuilt with ASan/UBSan; the four
actual handler/builder cases passed with 1,483 stable source/header inputs.
Copied raw-policy, requested-wallet-binding and partial-builder-publication
controls failed their intended assertions; restored four cases passed. Initial
fixture include and target-link failures were corrected and retained privately.

Normal link discovery uses archives; instrumented/control maps contain no
project C++ archive members. External libraries, Rust, C and PQClean remain
uninstrumented, macOS LSan is off, and full ARM RocksDB qualification is open.
The daemon, OFF configuration and other wallet test binaries are outside that
264-file instrumentation graph. The new required CI lane retains all 92 prior
CTest commands. Exact-source Linux qualification remains pending.
