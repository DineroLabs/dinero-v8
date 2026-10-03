# Service tip publication after durable state

## Problem and change

The shared `ChainstateService::PublishActiveTipLocked` setter logged before
publishing its active index pointer and observer identity. Log formatting can
allocate and throw. A caller that has already committed chainstate must not
return with the old service tip merely because diagnostics failed.

The setter still requires the actual activation mutex. It now acquires the
observer mutex before either representation changes, copies the index pointer
and fixed-size hash/height/validity fields together, then releases the observer
mutex. A synchronization/publication exception terminates the process rather
than returning to a possibly committed caller. Logging happens afterwards;
diagnostic exceptions are contained and no second allocating log is attempted.
Normal logging and all publication reasons are preserved. The outer wrapper's
activation-lock acquisition and the explicit caller-lock assertion retain their
existing behavior; this is not a new transaction or locking API.

## Regression scope

The existing independent `OrchardServiceStartup` executable exercises the real
service setter using generated indexes after its honest temporary-store fixture
has been committed and reopened. A test-executable-only allocator refuses every
ordinary C++ allocation on the publishing thread. It checks advancement,
rollback, null-tip publication and republishing, and checks both the active
pointer and observer hash/height/validity. For non-null tips it also requires an
actual refused allocation, so the diagnostic-failure path cannot pass vacuously.
The refusal ends before assertions or service destruction. Other threads keep
normal allocation behavior. No allocation hook enters the library or daemon.

The unchanged setter fails this regression because the diagnostic exception
escapes. The corrected setter passes. This test covers ordinary C++ allocation
failure in tip publication, not all allocators, mutex implementation failures,
or arbitrary process crashes. Existing persistence/startup and compatibility
lanes remain required.

## Remaining integration

This is the shared service publication operation, not a new Orchard connection
entry point. Runtime ConnectTip/DisconnectTip still need typed routing, complete
validation, the indexed commit owner, selected-history retirement context and
typed notification consumers. Other post-commit work must still be audited for
exceptions and consistency. Whole-daemon lifecycle and release-binary provenance
are separate gates. No network activation or production datadir is changed.
