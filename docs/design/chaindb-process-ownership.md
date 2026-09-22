# ChainDB ownership when the platform Env does not lock files

The iOS RocksDB Env returns a dummy file lock. The safe opener already keeps
that lock from preflight through database destruction, but a dummy lock cannot
exclude another ChainDB owner. Native tests using the same no-exclusion behavior
reproduced concurrent opening, alias opening and opening during final unlock.

## Boundary

On POSIX platforms, ChainDB now reserves the directory's device/inode identity
before asking the Env for LOCK or inspecting CURRENT. A retained directory
descriptor pins that identity. A process-wide registry rejects another ChainDB
owner of the same directory, including through a symlink or renamed path.
Independent directories remain usable concurrently.

The reservation belongs to the opener's RAII Env. It follows move construction
and assignment, outlives CF handles and the RocksDB instance, and is released
after the underlying Env finishes unlocking. Failed initialization releases it;
a failed file-lock attempt also releases it before retrying. There is no global
mutex held during database I/O, shutdown, a callback or a Swift await.

This adds one directory descriptor per open ChainDB. Windows retains the existing
native RocksDB locking path; this change does not replace that implementation.
The iOS no-op Env is unchanged. Physical-device behavior still needs qualification.

## What remains separate

This excludes **ChainDB objects**, not arbitrary raw RocksDB users or filesystem
tools. It does not fence another process when that platform disables file locks.
It cannot prevent directory removal or other external mutation of an open store.
No reset, migration or deletion permission follows from acquiring or releasing it.

In particular, ownership ends after database closure. The stop-to-maintenance gap
still requires the NodeCore recovery lease and durable recovery protocol. This
patch does not enable the read-only maintenance prototype or implement SR-1,
preserving recovery, activation, migration promotion or a Swift FFI adapter.

## Regression evidence

`ChainDBOpenSafety` retains its existing Linux CI lane and adds eight scenarios
against real, generated RocksDB stores with a no-lock Env:

- simultaneous owners and a directory symlink;
- entry during CURRENT preflight;
- entry while final UnlockFile is still executing;
- release after failed opening, then retry;
- move construction/assignment, including release of the destination's old store;
- a competing thread at a deterministic preflight barrier;
- a renamed directory referring to the same underlying inode;
- successful retry after the underlying Env initially refuses its file lock.

The initial six-case run on unchanged production code failed five cases. The
failed-open/retry control already passed. After the fix, all 33 opening cases pass
on native macOS, using freshly compiled ChainDB/test objects with existing native
dependency archives. Both removal of duplicate-owner rejection and premature
release before UnlockFile produce behavioral failures in compiling executables.
Device and simulator ChainDB translation units compile against the iOS SDK;
this is compilation evidence, not an on-device run or a fully linked release.

All fixtures are disposable. No production database, device or wallet is opened.
Linux CI and final combined release/device qualification remain required.
