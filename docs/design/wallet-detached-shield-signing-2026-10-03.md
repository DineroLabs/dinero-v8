# Shield signing with restored catalog ownership

Shield signing captures the complete present authenticated catalog in a checked FULL transaction and releases wallet, seed and SQLite ownership before restoring account state against immutable replay points. Preparation verifies the reserved operation and its exact shield request against the captured identity. The actual service has released selected-chain ownership after capturing the coin snapshot.

A new sole wallet lease and FULL transaction recheck every captured account and reached archive. The signer then requires the current executor owner, exact proof, request, unsigned transaction and coin-snapshot digest. Existing signing-key resolution and per-input signature rules remain in use. The signing seed is released before a second complete catalog recheck, and checked COMMIT precedes returning private signature bytes.

The signer makes no Ready or history writes, retires no proof, and releases no reservation. The service retains signatures privately until its separate selected-chain validation and Ready/history commit. Returned signatures alone do not establish admission, relay, mining or release readiness.

## Tests and remaining scope

Five component cases cover restoration without wallet/SQL ownership, exact real-input signing and later service completion, required read/key/COMMIT refusal, changed accounts, borrowed owners, lock/source refusal, missing executor work and request/snapshot mismatch. Registration is enabled with a 180-second deadline. Qualification results are recorded separately; this document claims no executed test result.

Ordinary payment and reservation writers retain their separate remaining ownership work. Complete backup/deletion protection, broad platform/load and release qualification remain open. Mainnet activation remains unset.
