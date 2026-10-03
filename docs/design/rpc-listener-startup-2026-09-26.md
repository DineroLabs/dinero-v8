# RPC listener startup qualification — 2026-09-26

## Behavior

`HttpRpcServer::start` now binds and listens synchronously before starting the
accept thread. Bind/listen or thread creation failure propagates to the existing
`RPCService::Start` failure path, so daemon startup cannot publish RPC readiness
for an unavailable listener. The accept thread owns the descriptor after
successful creation; failed creation closes it. A lifecycle mutex serializes
start/stop, and a stopped thread must be joined before restart.

This removes the former fixed sleep as a readiness assumption. A listening
socket does not certify every RPC method or application subsystem. The existing
connection-handler drain policy is unchanged; this is not general shutdown or
concurrent destruction qualification.

## Executable qualification

The required `RPCListenerStartup` CTest reserves the actual configured RPC port
while launching an isolated daemon. It requires nonzero startup failure and no
readiness announcement. After releasing the socket, it reopens the same datadir
on the same port, performs a cookie-authenticated `getblockcount`, invokes `stop`,
and requires a clean exit. It does not retry port conflicts. The lane is enabled
in the serial daemon suite, Orchard workflow, and compact daemon sanitizer suite;
the sanitizer evidence verifier requires its execution. Existing test assertions
and deadlines are unchanged.

The preceding daemon failed the new regression before the implementation change.
A fresh full daemon build and the declared CTest pass. All 406 linked project C++
translation units were freshly ASan/UBSan instrumented and the actual isolated
daemon regression passed. Link maps exclude uninstrumented project C++ archive
members. External libraries, Rust, C and Objective-C++ are outside this scope;
macOS leak detection is disabled. This does not close the separate ARM RocksDB
sanitizer gate. Inherited build labels/dependencies are not release provenance.

A copied-source control that hides bind failure failed the false-readiness
assertion without sanitizer diagnostics; the restored instrumented daemon passed again. This regression
establishes accurate startup failure; it does not identify or prevent another
process from occupying a port, nor qualify Windows or a complete node lifecycle.

## Release scope

Orchard baseline reconciliation, production notification/recovery installation,
independently validated activation history and startup/replay/reindex remain
separate unfinished release requirements. No activation or deployment changes.
