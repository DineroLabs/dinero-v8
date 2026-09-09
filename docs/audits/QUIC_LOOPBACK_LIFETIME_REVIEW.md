# QUIC loopback fixture lifetime race

PR #721's QuicSessionStress aborted on Linux with an unaligned tcache chunk.
A passing retry did not resolve it. Linux GCC ThreadSanitizer reproduced an
unsynchronized shared_ptr read in the packet writer against reset() in test
teardown. Close() requests termination; it does not join the session thread.
Consequently handshake completion and Close() do not protect the peer pointer.

Both loopback fixtures now own their endpoints through QuicLoopbackPair. Packet
delivery holds a routing mutex while enqueuing; teardown takes that mutex,
removes both destinations and releases it before joining/destroying sessions.
Callbacks capture routing state by value, never references to reset pointers.
RAII applies the same cleanup on timeouts and assertion exits. An additional
case closes 100 pairs during handshake without waiting for readiness.

Linux TSan: original fixture reports the race; patched fixture passes three
rounds of 1000 completed handshakes plus 100 early teardowns. ASan verification
is also running. Production QUIC code was not modified. This establishes the
fixture defect, not a general claim that all QUIC code is race-free.

The QUIC CI lane now requires TSan on both session executables and retains
logs. setarch -R limits its address-layout workaround to test descendants;
no host-wide kernel setting changes. Sanitizer startup failures are not skipped.

Evidence: shielded-integration-evidence/fleet-8038aeba6/quic/ on the review host.
