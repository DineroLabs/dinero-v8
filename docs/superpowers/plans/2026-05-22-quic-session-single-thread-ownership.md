# QuicSession Single-Thread Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `dinero::network::QuicSession` exclusive single-thread ownership of its `ngtcp2_conn` state, so listen-thread packet ingress, handler-thread send egress, and timer-driven `HandleExpiry` calls all serialize through a dedicated session-owned thread. Eliminate the multi-threaded race that broke v1–v4 band-aid attempts (`ece89351 → fa80cbc3` on `fix/quic-handshake-race`).

**Architecture:** Each `QuicSession` instance owns one `std::thread` that exclusively calls all `ngtcp2_conn_*` functions and the `DrainOutgoing` path. Other threads communicate with it through three queues: (1) incoming-packet inbox populated by the listen path; (2) outgoing-stream inbox populated by the send path; (3) decrypted-stream outbox drained by the handler path. Handshake completion is signaled through a `std::shared_future<bool>` so the handler thread can `wait_for()` instead of polling. An `OutboundWriter` callback hands wire-bytes back to the relay layer.

**Tech Stack:** C++17, `ngtcp2` (OpenSSL crypto backend), `std::thread`, `std::deque + std::mutex + std::condition_variable` (matches existing house style in `src/utils/activity_bus.cpp` — no new primitive needed), `std::promise / std::shared_future`, GoogleTest.

---

## Background

### What's broken

`QuicSession` (defined in `include/network/quic_session.h`, implemented in `src/network/quic_session.cpp`) wraps `ngtcp2_conn` but is **not internally thread-safe** — its public API requires external synchronization. Currently the relay subsystem in `src/daemon/p2p_manager.cpp` calls `QuicSession` methods from two threads:

- **Listen thread** (`unwrap_relay_quic_packet` at `p2p_manager.cpp:1292`) — on every inbound `RELAY_DATA` frame: `StartServerFromInitial` (first packet) → `ReceivePacket` → `HandleExpiry` → `drain_relay_quic_outgoing`. Holds `peer.relay_quic_mutex`.
- **Handler thread** (`peer_handler_loop` at `p2p_manager.cpp:4328`) — pre-handshake: polls `handshake_ready()`; post-handshake: calls `send_relay_data_to_virtual_peer` (`p2p_manager.cpp:5520`) which calls `QueueStreamData` + `drain_relay_quic_outgoing`. Holds the same mutex.

Four band-aid attempts on branch `fix/quic-handshake-race` all failed:

| Commit | What | Result |
|--------|------|--------|
| `ece89351` (v1) | Handler polls + drives drain/expiry, NO lock | One direction worked, the other timed out |
| `c2668ac9` (v2) | Spawn handler on inbound too | Both sides timeout |
| `755ab81c` (v3) | Drop drain/expiry from handler, just poll ready | Both timeout — retransmits never fire |
| `fa80cbc3` (v4) | Re-add drain/expiry under `relay_quic_mutex` | Both still timeout |

The empirical conclusion (from `fa80cbc3` commit message): mutex discipline is too fragile because two threads still drive `ngtcp2` internal state. The library's internal state machine assumes single-thread ownership.

### Why this fix

`ngtcp2` is designed for a single-owner event loop. Forcing that model structurally — by giving `QuicSession` its own thread — makes the race class impossible by construction rather than fragile-by-convention. This is the same shape as the Apr 30 chainstate hardening (`efbc5b63a` + `ChainstateCommitBatch`): replace runtime-fragile invariants with compile-time-enforced ones.

### Out of scope

- Replacing `ngtcp2` with a different QUIC library.
- Restructuring the `peer_handler_loop` beyond the minimal changes to use the new wait/read primitives.
- Changing the wire protocol of `RELAY_DATA` frames.
- Any change to the plaintext (non-QUIC) relay path.

### Invariants the refactor must preserve

1. `peer.relay_quic_stream_buffer` continues to carry decrypted application bytes to the handler thread's `receive_peer_message` path.
2. `enqueue_relay_frame` continues to receive complete `P2PMessage` frames extracted from the decrypted stream.
3. `send_relay_payload_to_virtual_peer` remains the wire-bytes-out path that wraps in `RELAY_DATA` and sends via the relay socket.
4. The 10s handshake-wait timeout (matches `kRelayConnectTimeout`) is preserved — a tunnel that can't handshake in that window is dead.
5. Non-QUIC and non-virtual peers are completely unaffected.

### Branching strategy

The four band-aid commits live on `fix/quic-handshake-race` and were **never merged** to `dinero-main`. Do NOT branch from `fix/quic-handshake-race`. Branch from `dinero-main` (current tip `e098024b`, which is PR #124). The band-aid branch can be abandoned (or kept for archaeology).

---

## File Structure

**Modified files:**

| File | Responsibility |
|------|----------------|
| `include/network/quic_session.h` | Public API extended: owning thread, `OutboundWriter`, `WaitHandshakeReady`, `ReadDecryptedStream`. Old synchronous API removed. |
| `src/network/quic_session.cpp` | Internal event loop (`session_loop`) added; all `ngtcp2_conn_*` calls move inside it; existing helpers become enqueue-only on the public side. |
| `src/daemon/p2p_manager.cpp` | Listen path simplified to enqueue; handler-thread polling deleted; send path simplified to enqueue; `peer.relay_quic_mutex` field removed; test helpers updated. |
| `include/daemon/p2p_manager.h` | `peer.relay_quic_mutex` field removed from `PeerInfo`; `relay_quic_session` lifetime stays `shared_ptr<QuicSession>`. |
| `tests/network/test_quic_session.cpp` | `LoopbackHandshakeAndOneEncryptedStreamPayload` test migrated to the new threaded API. |

**New files:**

| File | Responsibility |
|------|----------------|
| `tests/network/test_quic_session_stress.cpp` | 1000-iteration stress test gating the refactor's success. |

**No new utility files** — `std::deque + std::mutex + std::condition_variable` is the existing house pattern (`src/utils/activity_bus.cpp:14-15`); inline it in `quic_session.cpp`.

---

## Task 1: Branch setup and stress test gate

**Files:**
- Test: `tests/network/test_quic_session_stress.cpp` (create)
- Build: whatever CMake registers tests in this codebase (likely `tests/CMakeLists.txt` or `tests/network/CMakeLists.txt`)

- [ ] **Step 1: Create an isolated worktree (do NOT touch the main worktree's branch)**

This repo runs concurrent agent sessions on shared branches (memory: `codex_concurrent_worktree`). All PR work belongs in a dedicated worktree under `/private/tmp/dinero-v8-*`, not the main checkout.

```bash
cd /Users/haydarevich/src/dinero-v8
git fetch origin
git worktree add /private/tmp/dinero-v8-pr126 -b fix/quic-session-single-thread-ownership origin/dinero-main
cd /private/tmp/dinero-v8-pr126
git log -1 --oneline
```

Expected: HEAD at `e098024b refactor(p2p): name relay state by what it is, not what triggers it (#124)`.

**All subsequent task steps run from `/private/tmp/dinero-v8-pr126` unless explicitly noted.**

- [ ] **Step 2: Write the stress test (this test will not compile yet — that is correct for TDD)**

The test exercises the **new** API. It must fail to compile (or fail at runtime) until Tasks 2–6 are done. This is the gating success criterion for the refactor.

Create `tests/network/test_quic_session_stress.cpp`:

```cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/quic_session.h"
#include "network/quic_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Cert/key reused from test_quic_session.cpp — kept inline so the stress
// test can be moved/run independently.
constexpr auto kStressPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgwEvkGGgXAcRaG7Z8
gA7C6+W2RsW9gcjV9e5ybr0ikaahRANCAASCo35bDi+Q/q/CzHI1e5QaBrbqbFhW
G20QbVAeMK8l0oC8OGD3PSpZK1HXwALwzhMuwhxDos3ANb5naa5y17fQ
-----END PRIVATE KEY-----
)";

constexpr auto kStressCertificate = R"(-----BEGIN CERTIFICATE-----
MIICBzCCAa2gAwIBAgIUd2l6Pce3S0QH3dQC0Q/CjHbmggowCgYIKoZIzj0EAwIw
WTELMAkGA1UEBhMCQVUxEzARBgNVBAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGElu
dGVybmV0IFdpZGdpdHMgUHR5IEx0ZDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI1
MTExNDExNTcwMFoXDTI1MTIxNDExNTcwMFowWTELMAkGA1UEBhMCQVUxEzARBgNV
BAgMClNvbWUtU3RhdGUxITAfBgNVBAoMGEludGVybmV0IFdpZGdpdHMgUHR5IEx0
ZDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE
gqN+Ww4vkP6vwsxyNXuUGga26mxYVhttEG1QHjCvJdKAvDhg9z0qWStR18AC8M4T
LsIcQ6LNwDW+Z2mucte30KNTMFEwHQYDVR0OBBYEFFVgXLoLwzpf6+twP5z8Ujr2
5mxnMB8GA1UdIwQYMBaAFFVgXLoLwzpf6+twP5z8Ujr25mxnMA8GA1UdEwEB/wQF
MAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhAO4tnDNRAcooz62vf2m7vTyDqFCjcaIv
SJ9Gq0lvEXEcAiBwWBNUASBqLaje3hmtgwxcF7EIqqiGo5j8f9Ufgu6SRg==
-----END CERTIFICATE-----
)";

dinero::network::UdpAddr Localhost(uint16_t port) {
    const uint8_t ip[4] = {127, 0, 0, 1};
    return dinero::network::UdpAddr::FromIPv4(ip, port);
}

dinero::network::QuicSessionOptions StressOptions() {
    dinero::network::QuicSessionOptions options;
    options.alpn = "dinero-relay-test/1";
    options.server_name = "localhost";
    options.certificate_pem = kStressCertificate;
    options.private_key_pem = kStressPrivateKey;
    options.verify_peer = false;
    return options;
}

// Run one full client/server handshake with the new threaded API; return
// true iff both sides reach handshake_ready within `timeout`.
bool RunOneHandshake(std::chrono::milliseconds timeout) {
    using dinero::network::QuicSession;

    const auto client_addr = Localhost(0);  // ephemeral; loopback wiring is logical
    const auto server_addr = Localhost(0);

    std::shared_ptr<QuicSession> client_session;
    std::shared_ptr<QuicSession> server_session;

    // OutboundWriter forwards wire bytes to the peer's incoming queue.
    auto client_writer = [&server_session](std::vector<uint8_t> bytes) {
        if (server_session) {
            server_session->EnqueueIncomingPacket(std::move(bytes));
        }
    };
    auto server_writer = [&client_session](std::vector<uint8_t> bytes) {
        if (client_session) {
            client_session->EnqueueIncomingPacket(std::move(bytes));
        }
    };

    server_session = std::make_shared<QuicSession>(server_writer);
    client_session = std::make_shared<QuicSession>(client_writer);

    if (!server_session->StartServer(server_addr, client_addr, StressOptions())) {
        return false;
    }
    if (!client_session->StartClient(client_addr, server_addr, StressOptions())) {
        return false;
    }

    auto client_ready = client_session->WaitHandshakeReady();
    auto server_ready = server_session->WaitHandshakeReady();

    if (client_ready.wait_for(timeout) != std::future_status::ready) return false;
    if (server_ready.wait_for(timeout) != std::future_status::ready) return false;

    return client_ready.get() && server_ready.get();
}

}  // namespace

TEST(QuicSessionStress, OneThousandLoopbackHandshakesAllSucceed) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    constexpr int kIterations = 1000;
    constexpr auto kPerHandshakeTimeout = std::chrono::seconds(5);

    int successes = 0;
    int failures = 0;
    for (int i = 0; i < kIterations; ++i) {
        if (RunOneHandshake(kPerHandshakeTimeout)) {
            ++successes;
        } else {
            ++failures;
        }
    }

    EXPECT_EQ(successes, kIterations) << "stress test failures: " << failures
                                      << " of " << kIterations;
}
```

- [ ] **Step 3: Register the new test in CMake**

Find the test registration block — likely in `tests/network/CMakeLists.txt` or near the existing `test_quic_session` registration. Add an analogous `add_executable` / `add_test` entry for `test_quic_session_stress`. If unsure, search for the registration of `test_quic_session`:

```bash
grep -rn "test_quic_session" tests/ CMakeLists.txt 2>/dev/null | grep -i cmake
```

Copy the matching block and replace the filename. Whichever GoogleTest harness it uses (likely `add_test` with `gtest_discover_tests`), follow the same pattern.

- [ ] **Step 4: Verify the test fails to compile**

```bash
cmake -S . -B build-check
cmake --build build-check --target test_quic_session_stress 2>&1 | tail -30
```

Expected: compile errors mentioning unknown methods on `QuicSession` — at minimum the constructor signature `QuicSession(writer)`, `StartServer(...)`, `EnqueueIncomingPacket(...)`, `WaitHandshakeReady()`. This is the correct failing-test state. If it compiles, the API surface assumption is wrong — STOP and re-read `include/network/quic_session.h` before proceeding.

- [ ] **Step 5: Commit**

```bash
git add tests/network/test_quic_session_stress.cpp tests/network/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test(quic): add 1000-iteration handshake stress test (failing — gates refactor)

Exercises the new threaded QuicSession API that PR #126 will introduce.
Compiles only after Tasks 2-6 land. Stress count is intentionally high
because the race this refactor closes is statistical — band-aid v1-v4
all "worked" in single-shot tests but failed under realistic timing.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Extend `QuicSession` header with the new threaded API

**Files:**
- Modify: `include/network/quic_session.h` (full rewrite of the class definition)

- [ ] **Step 1: Replace the class definition in `include/network/quic_session.h`**

Keep the file header, namespace, `QuicSessionOptions`, `QuicSessionStats` exactly as they are. Replace lines `34–74` (the `class QuicSession { ... }` block) with:

```cpp
class QuicSession {
public:
    // Wire-bytes-out callback. Invoked from the owning session thread.
    // Implementations MUST be non-blocking or at most briefly blocking
    // (they hand the bytes to the relay socket writer). If they block,
    // they stall only this session — that's correct semantics.
    using OutboundWriter = std::function<void(std::vector<uint8_t>)>;

    enum class Role {
        Client,
        Server,
    };

    // Construction starts the owning thread, which blocks on the inbox
    // until Start{Client,Server} is called. `writer` must be valid for the
    // lifetime of the session.
    explicit QuicSession(OutboundWriter writer);

    // Destructor signals the owning thread to stop, drains pending events,
    // and joins. Safe to call at any time.
    ~QuicSession();

    QuicSession(const QuicSession&) = delete;
    QuicSession& operator=(const QuicSession&) = delete;

    // Initiate handshake as client. Schedules work onto the owning thread
    // and returns. After this, the session thread will drive ngtcp2,
    // emit outgoing bytes via `writer`, and resolve the handshake future
    // when complete.
    bool StartClient(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& options = QuicSessionOptions{});

    // Prime the session for server role. Subsequent EnqueueIncomingPacket
    // calls will cause StartServerFromInitial to be invoked on the first
    // packet, then ReceivePacket on it and all subsequent packets.
    bool StartServer(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& options);

    // Deliver an inbound QUIC packet. Called by the relay listen thread
    // on every RELAY_DATA frame. Non-blocking; pushes to the inbox and
    // returns immediately.
    void EnqueueIncomingPacket(std::vector<uint8_t> packet);

    // Enqueue application-layer stream bytes to send. Called by the
    // peer-handler thread (via send_relay_data_to_virtual_peer).
    // Non-blocking.
    void QueueOutgoingStream(std::vector<uint8_t> payload, bool fin = false);

    // Handshake-completion future. Resolves true when both sides have
    // completed the QUIC handshake; resolves false on session failure,
    // close, or destruction. Safe to call from any thread; multiple
    // concurrent waiters are allowed (shared_future).
    std::shared_future<bool> WaitHandshakeReady();

    // Pop any decrypted application-layer bytes that the session thread
    // has emitted since the last call. Blocks up to `timeout` if nothing
    // is available. Returns empty vector on timeout or session close.
    std::vector<uint8_t> ReadDecryptedStream(std::chrono::milliseconds timeout);

    // Snapshot of state. Safe from any thread; reads atomic snapshot
    // fields the session thread publishes.
    QuicSessionStats Stats() const;

    bool active() const;
    bool handshake_ready() const;
    std::string last_error() const;

    // Initiate graceful close. The session thread will drain the inbox,
    // emit any pending CONNECTION_CLOSE, and exit.
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

Add to the top of the file (after existing includes, before the namespace):

```cpp
#include <functional>
#include <future>
#include <memory>
```

- [ ] **Step 2: Verify the header compiles standalone**

```bash
cmake --build build-check --target dinero_network 2>&1 | tail -20
```

Expected: link errors only (because the .cpp doesn't define the new symbols yet). No header parse errors.

- [ ] **Step 3: Commit**

```bash
git add include/network/quic_session.h
git commit -m "$(cat <<'EOF'
quic: extend QuicSession header with threaded ownership API

Adds OutboundWriter callback, StartServer(), EnqueueIncomingPacket(),
QueueOutgoingStream(), WaitHandshakeReady(), ReadDecryptedStream(),
Close(). Removes the old synchronous ReceivePacket/HandleExpiry/
DrainOutgoing/QueueStreamData/TakeReceivedStreamData surface — those
become internal to the session-owned thread.

Implementation follows in subsequent commits; this commit is
header-only and will link-error until Task 3 lands.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Implement the session thread event loop in `quic_session.cpp`

**Files:**
- Modify: `src/network/quic_session.cpp` (substantial rewrite of `Impl`)

This is the largest task. The existing `Impl` struct (currently at `quic_session.cpp:102+`) holds `ngtcp2_conn` and helpers. We're extending it with a dedicated thread that owns those fields exclusively, plus three queues and the handshake promise.

- [ ] **Step 1: Add new private members to `Impl`**

Inside the `Impl` struct definition, add these fields (placement: after the existing ngtcp2 / option fields, before any methods):

```cpp
// --- Single-thread ownership members ---
OutboundWriter outbound_writer;

std::thread session_thread;
std::atomic<bool> stopping{false};

std::mutex inbox_mutex;
std::condition_variable inbox_cv;
// Three queues, all guarded by inbox_mutex / signaled by inbox_cv.
std::deque<std::vector<uint8_t>> incoming_packets;
std::deque<std::pair<std::vector<uint8_t>, bool /*fin*/>> outgoing_streams;
enum class StartRequest { None, Client, Server };
StartRequest pending_start{StartRequest::None};
UdpAddr pending_local;
UdpAddr pending_remote;
QuicSessionOptions pending_options;

// Decrypted-stream outbox: consumed by handler thread via
// ReadDecryptedStream. Has its own lock to decouple from inbox waiters.
std::mutex outbox_mutex;
std::condition_variable outbox_cv;
std::deque<std::vector<uint8_t>> decrypted_outbox;

// Handshake completion future. Set once when handshake completes (true)
// or when session fails/closes before handshake (false).
std::promise<bool> handshake_promise;
std::shared_future<bool> handshake_future;
bool handshake_promise_set{false};  // session-thread-only field

// Atomic published-state snapshots (so const accessors are lock-free).
std::atomic<bool> active_published{false};
std::atomic<bool> handshake_ready_published{false};

// Last-error string with its own lock (rarely contended).
mutable std::mutex error_mutex;
std::string last_error_str;
```

The existing `ngtcp2_conn*`, options, stream buffers, etc., stay as they are but become **session-thread-only access** by convention.

- [ ] **Step 2: Constructor and destructor**

Replace the existing `QuicSession::QuicSession()` and `~QuicSession()` with:

```cpp
QuicSession::QuicSession(OutboundWriter writer)
    : impl_(std::make_unique<Impl>()) {
    impl_->outbound_writer = std::move(writer);
    impl_->handshake_future = impl_->handshake_promise.get_future().share();
    impl_->session_thread = std::thread([impl = impl_.get()]() {
        SessionLoop(*impl);
    });
}

QuicSession::~QuicSession() {
    Close();
    if (impl_->session_thread.joinable()) {
        impl_->session_thread.join();
    }
    // If we never set the promise, set it false now so any waiters wake.
    if (!impl_->handshake_promise_set) {
        try { impl_->handshake_promise.set_value(false); } catch (...) {}
    }
}
```

`SessionLoop` is a free function (file-scope, `static`) defined in the anonymous namespace of `quic_session.cpp`. Skeleton (Step 3 fleshes it out):

```cpp
namespace {

void SessionLoop(QuicSession::Impl& impl) {
    // Main event loop runs until stopping flag set AND inbox drained.
    while (true) {
        // 1. Wait for next event with timeout = ngtcp2 expiry deadline.
        std::unique_lock<std::mutex> lock(impl.inbox_mutex);
        const auto wakeup = ComputeNextWakeup(impl);
        impl.inbox_cv.wait_until(lock, wakeup, [&]() {
            return impl.stopping.load() ||
                   impl.pending_start != QuicSession::Impl::StartRequest::None ||
                   !impl.incoming_packets.empty() ||
                   !impl.outgoing_streams.empty();
        });

        // 2. Drain queues into local vectors so we can release the lock.
        const auto start_req = impl.pending_start;
        impl.pending_start = QuicSession::Impl::StartRequest::None;
        UdpAddr start_local = impl.pending_local;
        UdpAddr start_remote = impl.pending_remote;
        QuicSessionOptions start_options = impl.pending_options;

        std::deque<std::vector<uint8_t>> incoming;
        std::swap(incoming, impl.incoming_packets);
        std::deque<std::pair<std::vector<uint8_t>, bool>> outgoing;
        std::swap(outgoing, impl.outgoing_streams);
        const bool stopping = impl.stopping.load();
        lock.unlock();

        // 3. Handle Start request (first packet for server, immediate for client).
        if (start_req == QuicSession::Impl::StartRequest::Client) {
            DoStartClient(impl, start_local, start_remote, start_options);
        } else if (start_req == QuicSession::Impl::StartRequest::Server) {
            // Save options for first-packet StartServerFromInitial.
            impl.pending_options = std::move(start_options);
            impl.pending_local = start_local;
            impl.pending_remote = start_remote;
            // Server doesn't start ngtcp2 yet — waits for first incoming packet.
        }

        // 4. Process incoming packets.
        for (auto& packet : incoming) {
            ProcessIncomingPacket(impl, packet);
        }

        // 5. Process outgoing stream data.
        for (auto& [payload, fin] : outgoing) {
            ProcessOutgoingStream(impl, std::move(payload), fin);
        }

        // 6. Run expiry (advances retransmit timers).
        if (impl.active_published.load()) {
            RunHandleExpiry(impl);
        }

        // 7. Drain outgoing wire bytes and ship via writer.
        DrainAndShip(impl);

        // 8. Check handshake completion → fulfill promise once.
        MaybePublishHandshakeReady(impl);

        // 9. Pump decrypted stream data into outbox.
        PublishDecryptedToOutbox(impl);

        if (stopping && impl.incoming_packets.empty() && impl.outgoing_streams.empty()) {
            break;
        }
    }
}

}  // namespace
```

- [ ] **Step 3: Implement the helper functions called by `SessionLoop`**

Each helper is single-threaded (only the session thread calls it). They wrap the existing `ngtcp2_conn_*` calls. Implementation is mostly **moving existing logic** out of the current `StartClient`, `StartServerFromInitial`, `ReceivePacket`, `HandleExpiry`, `DrainOutgoing`, `QueueStreamData` into private helpers — those existing methods become enqueue-shims on the public API (Step 4).

```cpp
namespace {

// CRITICAL: ngtcp2 timestamps are uint64_t nanoseconds in ngtcp2's clock
// domain (set at ngtcp2_conn_*_new time), NOT steady_clock nanoseconds. The
// existing quic_session.cpp has a "now in ngtcp2 ns" helper (used for
// HandleExpiry and ReceivePacket); find it via `grep "ngtcp2_tstamp\|now_ns\|
// CurrentNgtcpTimestamp" src/network/quic_session.cpp`. The function below
// converts an absolute ngtcp2-domain expiry to a steady_clock wakeup deadline.
// Getting this wrong silently produces the v3 symptom: handshake_ready never
// flips because retransmits fire at the wrong wall-clock time.

std::chrono::steady_clock::time_point ComputeNextWakeup(QuicSession::Impl& impl) {
    if (!impl.active_published.load() || !impl.conn) {
        // No ngtcp2 conn yet — wait indefinitely (the cv predicate covers wake-up).
        return std::chrono::steady_clock::time_point::max();
    }
    const auto expiry_ns = ngtcp2_conn_get_expiry(impl.conn);
    if (expiry_ns == UINT64_MAX) {
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
    return NgtcpExpiryToSteady(impl, expiry_ns);
}

std::chrono::steady_clock::time_point NgtcpExpiryToSteady(
        QuicSession::Impl& impl, uint64_t expiry_ngtcp_ns) {
    const auto now_steady = std::chrono::steady_clock::now();
    // Replace CurrentNgtcpTimestampNs() with the actual helper name found via
    // the grep above. It's the same function the existing code passes to
    // ngtcp2_conn_handle_expiry as the timestamp argument.
    const uint64_t now_ngtcp = CurrentNgtcpTimestampNs();
    if (expiry_ngtcp_ns <= now_ngtcp) return now_steady;
    return now_steady + std::chrono::nanoseconds(expiry_ngtcp_ns - now_ngtcp);
}

void DoStartClient(QuicSession::Impl& impl,
                   const UdpAddr& local, const UdpAddr& remote,
                   const QuicSessionOptions& options) {
    // Move existing StartClient body here verbatim — same ngtcp2_conn_client_new,
    // same crypto setup, same options handling. Set impl.active_published = true
    // on success.
    // ... (existing body)
}

void ProcessIncomingPacket(QuicSession::Impl& impl,
                           const std::vector<uint8_t>& packet) {
    if (!impl.active_published.load()) {
        // Server role: this is the first packet. Init ngtcp2 server.
        if (!DoStartServerFromInitial(impl, packet)) {
            return;  // last_error_str already set
        }
    }
    // Move existing ReceivePacket body here.
    // ... (existing body, minus the public API entry-point checks)
}

void ProcessOutgoingStream(QuicSession::Impl& impl,
                           std::vector<uint8_t> payload, bool fin) {
    // Move existing QueueStreamData body here.
    // ... (existing body)
}

void RunHandleExpiry(QuicSession::Impl& impl) {
    // Move existing HandleExpiry body here.
    // ... (existing body)
}

void DrainAndShip(QuicSession::Impl& impl) {
    std::vector<std::vector<uint8_t>> packets;
    // Move existing DrainOutgoing body here. After draining, hand each packet
    // to impl.outbound_writer.
    // ... (existing drain body) ...
    for (auto& pkt : packets) {
        impl.outbound_writer(std::move(pkt));
    }
}

void MaybePublishHandshakeReady(QuicSession::Impl& impl) {
    if (impl.handshake_promise_set) return;
    if (!impl.conn) return;
    if (ngtcp2_conn_get_handshake_completed(impl.conn)) {
        impl.handshake_ready_published.store(true);
        impl.handshake_promise.set_value(true);
        impl.handshake_promise_set = true;
    }
}

void PublishDecryptedToOutbox(QuicSession::Impl& impl) {
    // Move existing TakeReceivedStreamData body here, but instead of returning,
    // push the bytes onto impl.decrypted_outbox under impl.outbox_mutex and
    // notify impl.outbox_cv.
    auto bytes = TakeReceivedStreamDataInternal(impl);
    if (bytes.empty()) return;
    {
        std::lock_guard<std::mutex> lock(impl.outbox_mutex);
        impl.decrypted_outbox.push_back(std::move(bytes));
    }
    impl.outbox_cv.notify_all();
}

}  // namespace
```

- [ ] **Step 4: Reimplement public API as enqueue-shims**

The public API methods now only enqueue work and return. They are non-blocking and thread-safe by virtue of `inbox_mutex`.

```cpp
bool QuicSession::StartClient(const UdpAddr& local, const UdpAddr& remote,
                              const QuicSessionOptions& options) {
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->pending_start = Impl::StartRequest::Client;
        impl_->pending_local = local;
        impl_->pending_remote = remote;
        impl_->pending_options = options;
    }
    impl_->inbox_cv.notify_one();
    return true;  // session thread reports failure via last_error_str + future
}

bool QuicSession::StartServer(const UdpAddr& local, const UdpAddr& remote,
                              const QuicSessionOptions& options) {
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->pending_start = Impl::StartRequest::Server;
        impl_->pending_local = local;
        impl_->pending_remote = remote;
        impl_->pending_options = options;
    }
    impl_->inbox_cv.notify_one();
    return true;
}

void QuicSession::EnqueueIncomingPacket(std::vector<uint8_t> packet) {
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->incoming_packets.push_back(std::move(packet));
    }
    impl_->inbox_cv.notify_one();
}

void QuicSession::QueueOutgoingStream(std::vector<uint8_t> payload, bool fin) {
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->outgoing_streams.emplace_back(std::move(payload), fin);
    }
    impl_->inbox_cv.notify_one();
}

std::shared_future<bool> QuicSession::WaitHandshakeReady() {
    return impl_->handshake_future;
}

std::vector<uint8_t> QuicSession::ReadDecryptedStream(
        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->outbox_mutex);
    impl_->outbox_cv.wait_for(lock, timeout, [&]() {
        return !impl_->decrypted_outbox.empty() || impl_->stopping.load();
    });
    if (impl_->decrypted_outbox.empty()) return {};
    auto out = std::move(impl_->decrypted_outbox.front());
    impl_->decrypted_outbox.pop_front();
    return out;
}

bool QuicSession::active() const {
    return impl_->active_published.load();
}

bool QuicSession::handshake_ready() const {
    return impl_->handshake_ready_published.load();
}

std::string QuicSession::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->last_error_str;
}

void QuicSession::Close() {
    impl_->stopping.store(true);
    impl_->inbox_cv.notify_all();
    impl_->outbox_cv.notify_all();
}

QuicSessionStats QuicSession::Stats() const {
    // Build snapshot from atomic-published fields. Avoid touching session-thread-
    // only fields like impl_->conn. Reuse the existing Stats() body but read
    // from impl_->active_published, impl_->handshake_ready_published, and any
    // other fields the session thread atomically publishes.
    QuicSessionStats stats;
    stats.active = impl_->active_published.load();
    stats.handshake_completed = impl_->handshake_ready_published.load();
    // ... copy other fields the existing Stats() returned ...
    return stats;
}
```

- [ ] **Step 5: Build**

```bash
cmake --build build-check --target dinero_network 2>&1 | tail -40
```

Expected: clean compile. If errors:
- Missing `NgtcpExpiryToSteady` / `TakeReceivedStreamDataInternal` — these are placeholder names for existing helpers in the file; rename to whatever they are actually called in `quic_session.cpp`.
- Missing `Impl::StartRequest` qualification — `StartRequest` is nested in `Impl`; make sure friend access or the public API can reach it (move to detail namespace if needed).

- [ ] **Step 6: Commit**

```bash
git add src/network/quic_session.cpp
git commit -m "$(cat <<'EOF'
quic: implement single-thread session loop owning all ngtcp2 state

QuicSession now owns one std::thread that exclusively calls every
ngtcp2_conn_* function. Public methods (StartClient, StartServer,
EnqueueIncomingPacket, QueueOutgoingStream, Close) enqueue work onto
the inbox and return immediately. ReadDecryptedStream blocks the
calling thread on an outbox condvar. WaitHandshakeReady returns a
shared_future fulfilled by the session thread when ngtcp2 reports
handshake_completed.

Removes the requirement that callers serialize access — the bug
class that broke v1-v4 band-aids on fix/quic-handshake-race
becomes impossible by construction.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Run the stress test against the new QuicSession

**Files:**
- Run only — no edits.

- [ ] **Step 1: Build the stress test**

```bash
cmake --build build-check --target test_quic_session_stress 2>&1 | tail -20
```

Expected: clean compile (header signatures from Task 2 now have implementations from Task 3).

- [ ] **Step 2: Run the stress test once**

```bash
./build-check/tests/network/test_quic_session_stress 2>&1 | tail -20
```

Expected: `[  PASSED  ] 1 test.` with 1000 successes, 0 failures.

If failures:
- Read the per-handshake timeout output. If <100% pass but most succeed → likely a missed `notify_one` or wakeup-compute bug; check `ComputeNextWakeup` returns sane values.
- If 0% pass → likely the writer-loopback wiring in the test is broken, OR the session thread never starts. Add a temporary `std::cout << "session loop iter" << std::endl;` at the top of `SessionLoop` and re-run to confirm the thread is actually scheduled.
- Do NOT add mutex band-aids. The whole point is to fix this structurally. If something races, find the cross-thread access and route it through the inbox.

- [ ] **Step 3: Run the stress test 5x in a row to verify it's not flaky**

```bash
for i in 1 2 3 4 5; do
  echo "=== run $i ==="
  ./build-check/tests/network/test_quic_session_stress 2>&1 | grep -E "(PASSED|FAILED|stress test failures)"
done
```

Expected: 5x `[  PASSED  ] 1 test.`

- [ ] **Step 4: Run the existing tests in `test_quic_session.cpp`**

Tests still referencing the old synchronous API will fail to compile — fix them in Task 8. For now skip those by name. First enumerate what's there:

```bash
grep -n "^TEST(" tests/network/test_quic_session.cpp
```

Build a `--gtest_filter` that excludes each one, e.g. `--gtest_filter='-QuicSession.LoopbackHandshakeAndOneEncryptedStreamPayload:-QuicSession.OtherTest:-...'`. Run remaining tests:

```bash
./build-check/tests/network/test_quic_session --gtest_filter='-QuicSession.*' 2>&1 | tail -10
```

(If the whole file's tests use the old API, the binary may not even link until Task 8.)

**Important caveat on what the stress test proves:** A direct in-process loopback writer does NOT reproduce the timing characteristics of a TCP-tunneled relay path (head-of-line blocking, kernel TCP buffering, real-world jitter). The stress test proves *code correctness of the new threaded design under synthetic conditions*. It does NOT prove the production race class is closed — that requires Task 10 canary soak. Do not ship on green stress test alone.

- [ ] **Step 5: Commit (test run only, no code change)**

No commit needed — this is a validation step. If results are green, proceed to Task 5.

---

## Task 5: Update the relay listen path in `p2p_manager.cpp`

**Files:**
- Modify: `src/daemon/p2p_manager.cpp:1292-1398` (`unwrap_relay_quic_packet`)

The current function takes `peer.relay_quic_mutex`, calls `StartServerFromInitial` / `ReceivePacket` / `HandleExpiry` / `drain_relay_quic_outgoing` directly, then extracts decrypted frames. After the refactor: the listen thread only enqueues the packet, and a separate path (Task 7) reads decrypted frames.

- [ ] **Step 1: Construct the QuicSession with the outbound writer on creation**

Find where `peer.relay_quic_session` is constructed. Currently at `p2p_manager.cpp:1303`:

```cpp
peer.relay_quic_session = std::make_shared<dinero::network::QuicSession>();
```

**Use the weak_ptr-capture variant** — NOT a lookup-by-virtual_peer_key writer. The lookup variant takes `peers_mutex_` on every outgoing packet (thousands/sec under transfer), serializing the entire peer table on a hot path. The weak_ptr variant has zero hot-path locks and clean lifetime semantics. Additionally, `send_relay_payload_to_virtual_peer` itself reads `peers_mutex_` near line `5485` (the stats update block); a lookup-variant writer would either deadlock or require recursive-mutex contortions.

Construct the session at a site where a `shared_ptr<PeerInfo>` is in hand (find one via the same pattern `start_peer_handler_thread` uses — `connected_peers_.find(virtual_peer_key)` then capture the shared_ptr). If `unwrap_relay_quic_packet` only has `PeerInfo&`, move the session construction up to the caller (e.g. `handle_relay_data`) where the shared_ptr is available.

```cpp
// At a site that has shared_ptr<PeerInfo> peer_sp in scope:
peer_sp->relay_quic_session = std::make_shared<dinero::network::QuicSession>(
    [this, peer_weak = std::weak_ptr<PeerInfo>(peer_sp)]
    (std::vector<uint8_t> bytes) {
        auto peer_locked = peer_weak.lock();
        if (!peer_locked) return;  // peer torn down — drop the packet
        send_relay_payload_to_virtual_peer(*peer_locked, bytes);
    });
```

Verify after wiring: confirm `send_relay_payload_to_virtual_peer` is reentrant-safe when called from a thread other than the listen thread, since the writer fires from the QuicSession-owned thread. Specifically check it doesn't assume single-thread invocation (e.g. doesn't touch thread-local state).

- [ ] **Step 2: After construction (server side), prime the session with StartServer**

Immediately after the construction in Step 1, add:

```cpp
if (!local_is_client) {
    if (!peer.relay_quic_session->StartServer(local_addr, remote_addr, *peer.relay_quic_options)) {
        std::cout << "[P2P] relay-transport: failed to start QUIC server for "
                  << peer.to_string() << ": "
                  << peer.relay_quic_session->last_error() << std::endl;
        return true;
    }
}
```

(Client-side `StartClient` is invoked elsewhere — wherever the current code initiates an outbound QUIC relay session. Find that callsite and add the writer there too. Likely near `peer.relay_quic_session->StartClient(...)` in the codebase.)

- [ ] **Step 3: Replace the mutex-guarded ngtcp2 block with enqueue**

Replace `p2p_manager.cpp:1300-1378` (the entire block from `std::lock_guard<std::mutex> lock(peer.relay_quic_mutex);` through the closing `}` of that block) with:

```cpp
if (!peer.relay_quic_session) {
    std::cout << "[P2P] relay-transport: no QUIC session for "
              << peer.to_string() << std::endl;
    return true;
}
peer.relay_quic_session->EnqueueIncomingPacket(packet);
```

That's it. The session thread will: process the packet, run expiry, drain outgoing (which calls our writer → `send_relay_payload_to_virtual_peer`), and push any decrypted bytes to its outbox. The handler thread (Task 6) reads from the outbox.

- [ ] **Step 4: Move the decrypted-frame extraction loop to a separate function**

The `while (!peer.relay_quic_stream_buffer.empty()) { ... PeekVarInt ... enqueue_relay_frame ... }` loop currently sits inside `unwrap_relay_quic_packet` (lines `1349-1377`). It needs to live on the handler thread now, reading from `QuicSession::ReadDecryptedStream` instead of from `peer.relay_quic_stream_buffer`.

Extract it as a private method on `P2PManager`:

```cpp
// in p2p_manager.h, private section:
void run_relay_quic_reader_loop(std::shared_ptr<PeerInfo> peer);

// in p2p_manager.cpp:
void P2PManager::run_relay_quic_reader_loop(std::shared_ptr<PeerInfo> peer) {
    std::vector<uint8_t> stream_buffer;
    const auto virtual_peer_key = peer->to_string();
    while (!shutdown_requested_.load() && peer->is_connected) {
        if (!peer->relay_quic_session) break;
        auto chunk = peer->relay_quic_session->ReadDecryptedStream(
            std::chrono::milliseconds(200));
        if (chunk.empty()) continue;
        stream_buffer.insert(stream_buffer.end(), chunk.begin(), chunk.end());

        while (!stream_buffer.empty()) {
            uint64_t frame_len = 0;
            size_t prefix_len = 0;
            if (!PeekVarInt(stream_buffer, 0, &frame_len, &prefix_len)) break;
            if (frame_len > 4ULL * 1024 * 1024) {
                std::cout << "[P2P] relay-transport: QUIC stream frame too large for "
                          << virtual_peer_key << std::endl;
                stream_buffer.clear();
                break;
            }
            const auto total_len = prefix_len + static_cast<size_t>(frame_len);
            if (stream_buffer.size() < total_len) break;
            std::vector<uint8_t> frame(
                stream_buffer.begin() + static_cast<std::ptrdiff_t>(prefix_len),
                stream_buffer.begin() + static_cast<std::ptrdiff_t>(total_len));
            stream_buffer.erase(
                stream_buffer.begin(),
                stream_buffer.begin() + static_cast<std::ptrdiff_t>(total_len));
            if (!P2PMessage::deserialize(frame)) {
                std::cout << "[P2P] relay-transport: dropped malformed decrypted QUIC frame for "
                          << virtual_peer_key << std::endl;
                continue;
            }
            if (!enqueue_relay_frame(virtual_peer_key, frame)) {
                std::cout << "[P2P] relay-transport: failed to queue decrypted QUIC frame for "
                          << virtual_peer_key << std::endl;
            }
        }
    }
}
```

This method runs on a new background thread spawned per QUIC virtual peer (Task 6 wires it up).

- [ ] **Step 5: Build and verify**

```bash
cmake --build build-check --target dinero_p2p 2>&1 | tail -30
```

Fix any compile errors. Common: `PeerInfo` field references — `peer.relay_quic_stream_buffer` and `peer.relay_quic_outbox_packets` will be removed in Task 7; leave them in place for now (the build will still succeed because they're unused after this task).

- [ ] **Step 6: Commit**

```bash
git add src/daemon/p2p_manager.cpp include/daemon/p2p_manager.h
git commit -m "$(cat <<'EOF'
p2p: route relay QUIC ingress through the session's owning thread

unwrap_relay_quic_packet no longer takes peer.relay_quic_mutex or
touches ngtcp2 directly. It enqueues the inbound packet onto the
QuicSession's inbox and returns. The session thread does StartServer
on first packet, ReceivePacket on every packet, expiry, and drain
(handing wire bytes to send_relay_payload_to_virtual_peer via the
OutboundWriter passed at construction).

Decrypted-frame extraction moves to run_relay_quic_reader_loop, which
will be spawned by start_peer_handler_thread for QUIC virtual peers in
the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Update the peer handler loop — wait on future, spawn reader

**Files:**
- Modify: `src/daemon/p2p_manager.cpp:4328-4422` (`peer_handler_loop`)

- [ ] **Step 1: Replace the polling block with future-wait**

The v1–v4 band-aids inserted a polling loop just before `perform_handshake`. On `dinero-main` (no band-aids), the code calls `perform_handshake` directly at line `4345`. After this refactor, **insert** a future-wait between the weak_ptr lock and the `perform_handshake` call:

```cpp
auto peer_locked = peer_weak.lock();
if (!peer_locked) { /* existing failure path */ }

// NEW: For QUIC-encrypted virtual peers, wait for the QUIC handshake to
// complete before invoking the dineroid app-layer handshake.
if (peer_locked->via_relay && peer_locked->via_relay->encrypted_quic) {
    if (!peer_locked->relay_quic_session) {
        std::cerr << "[P2P] relay-handshake: QUIC virtual peer has no session for "
                  << peer_key << std::endl;
        cleanup_peer(peer_key);
        return;
    }
    auto ready = peer_locked->relay_quic_session->WaitHandshakeReady();
    if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready ||
        !ready.get()) {
        std::cout << "[P2P] relay-transport: QUIC handshake did not become ready "
                  << "within 10s for " << peer_key << std::endl;
        cleanup_peer(peer_key);
        return;
    }
    std::cout << "[P2P] relay-handshake: QUIC handshake ready for "
              << peer_key << " — starting dineroid" << std::endl;

    // Spawn the decrypted-stream reader thread. It pumps decrypted bytes
    // from the QuicSession outbox into enqueue_relay_frame.
    {
        std::lock_guard<std::mutex> lock(peer_threads_mutex_);
        if (!shutdown_requested_.load(std::memory_order_acquire)) {
            peer_threads_.emplace_back(std::make_unique<std::thread>(
                &P2PManager::run_relay_quic_reader_loop, this, peer_locked));
        }
    }
}

if (!perform_handshake(peer_locked.get())) {
    /* existing failure path */
}
```

- [ ] **Step 2: Verify spawn-point for QUIC virtual peers**

In Task 5's commit, `unwrap_relay_quic_packet` created the QuicSession but did not spawn the handler thread for inbound virtual peers — that was the bug fixed by v2 (`c2668ac9`). Confirm the current `dinero-main` code at `p2p_manager.cpp:1287` does `start_peer_handler_thread(std::move(virtual_peer))` on the inbound path:

```bash
grep -n "start_peer_handler_thread" src/daemon/p2p_manager.cpp | head -10
```

If the spawn at line `1287` runs only on the plaintext path, port the v2 fix forward: add a spawn inside the QUIC creation branch in `handle_relay_data` (or wherever the virtual peer is added to `connected_peers_`). The v2 commit (`c2668ac9`) shows exactly the right insertion shape — read its diff for reference:

```bash
git show c2668ac9 -- src/daemon/p2p_manager.cpp | head -100
```

Port the spawn change but NOT the QUIC handshake polling from v1 — the polling is replaced by the future-wait in Step 1 above.

- [ ] **Step 3: Build**

```bash
cmake --build build-check --target dinerod 2>&1 | tail -20
```

- [ ] **Step 4: Commit**

```bash
git add src/daemon/p2p_manager.cpp
git commit -m "$(cat <<'EOF'
p2p: wait on QuicSession future instead of polling handshake_ready

peer_handler_loop now uses QuicSession::WaitHandshakeReady() and
shared_future::wait_for(10s) to block until the QUIC handshake
completes. No mutex needed — the future is fulfilled by the session
thread when ngtcp2 reports handshake_completed.

Also spawns run_relay_quic_reader_loop so decrypted application bytes
get drained from the session outbox and fed into enqueue_relay_frame
without touching ngtcp2 from this thread.

Equivalent ground covered by band-aids ece89351 + c2668ac9 + 755ab81c
+ fa80cbc3, structurally instead of via mutex discipline.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Update the relay send path

**Files:**
- Modify: `src/daemon/p2p_manager.cpp:5520-5557` (`send_relay_data_to_virtual_peer`)
- Modify: `include/daemon/p2p_manager.h` (remove `relay_quic_mutex` and `relay_quic_outbox_packets` from `PeerInfo`; remove `drain_relay_quic_outgoing` declaration)
- Modify: `src/daemon/p2p_manager.cpp` (remove the body of `drain_relay_quic_outgoing` at `5493-5518`; remove `relay_quic_stream_buffer` member)

- [ ] **Step 1: Simplify `send_relay_data_to_virtual_peer`**

Replace `p2p_manager.cpp:5520-5557` with:

```cpp
bool P2PManager::send_relay_data_to_virtual_peer(PeerInfo& peer,
                                                 const P2PMessage& message) {
    if (!peer.via_relay) return false;

    if (peer.via_relay->encrypted_quic) {
        if (!encrypted_relay_transport_allowed()) {
            std::cout << "[P2P] relay-transport: encrypted RELAY_DATA send refused until "
                      << "QUIC relay is enabled for this network for "
                      << peer.to_string() << std::endl;
            return false;
        }
        if (!peer.relay_quic_session || !peer.relay_quic_session->active() ||
            !peer.relay_quic_session->handshake_ready()) {
            std::cout << "[P2P] relay-transport: QUIC virtual peer is not handshake-ready for "
                      << peer.to_string() << std::endl;
            return false;
        }
        const auto inner_data = message.serialize();
        const auto framed = FrameRelayQuicStreamPayload(inner_data);
        peer.relay_quic_session->QueueOutgoingStream(framed, false);
        // No drain call — the session thread drains automatically after every
        // event. No mutex — QueueOutgoingStream is internally synchronized.
        return true;
    }

    if (!plaintext_relay_transport_allowed()) {
        std::cout << "[P2P] relay-transport: plaintext RELAY_DATA send refused on mainnet for "
                  << peer.to_string() << std::endl;
        return false;
    }
    const auto inner_data = message.serialize();
    return send_relay_payload_to_virtual_peer(peer, inner_data);
}
```

- [ ] **Step 2: Remove `drain_relay_quic_outgoing` entirely**

Delete the body at `p2p_manager.cpp:5493-5518`. Delete the declaration in `include/daemon/p2p_manager.h`. Find any remaining callers (Task 5 should have removed the listen-path call; there's no other production caller):

```bash
grep -n "drain_relay_quic_outgoing" src/ include/ tests/ 2>/dev/null
```

Expected: empty output. If anything remains, remove it.

- [ ] **Step 3: Remove the `relay_quic_mutex` field from `PeerInfo`**

In `include/daemon/p2p_manager.h`, find the `PeerInfo` struct and delete the `std::mutex relay_quic_mutex;` line. Then verify no callers remain:

```bash
grep -n "relay_quic_mutex" src/ include/ tests/ 2>/dev/null
```

Expected: empty.

- [ ] **Step 4: Remove `relay_quic_outbox_packets` field from `PeerInfo`**

It was a `#ifdef DINERO_TEST_BUILD` test-only deque used by `drain_relay_quic_outgoing` to capture undeliverable packets. With the new architecture, the session's `OutboundWriter` is the only outgoing path; if a test needs to capture, it should provide a writer that captures into the test's own structure. Delete the field and any `#ifdef`-guarded references to it.

- [ ] **Step 5: Remove `relay_quic_stream_buffer` field from `PeerInfo`**

The buffer was used by the listen thread (now removed in Task 5) and the post-handshake reader. The reader now uses a local buffer inside `run_relay_quic_reader_loop`. Delete the field.

- [ ] **Step 6: Build**

```bash
cmake --build build-check 2>&1 | tail -40
```

Fix compile errors. Common: test helpers `test_configure_relay_quic_server` / `test_drain_relay_quic_packets` / `test_relay_quic_handshake_ready` at `p2p_manager.cpp:3050-3113` reference the removed fields. Task 8 will fix those.

- [ ] **Step 7: Commit**

```bash
git add src/daemon/p2p_manager.cpp include/daemon/p2p_manager.h
git commit -m "$(cat <<'EOF'
p2p: route relay QUIC egress through the session's owning thread

send_relay_data_to_virtual_peer now calls QueueOutgoingStream and
returns. No mutex, no drain call — the session thread drains
automatically. PeerInfo loses relay_quic_mutex, relay_quic_stream_buffer,
and the test-only relay_quic_outbox_packets field. drain_relay_quic_outgoing
is deleted entirely.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Update test helpers and the existing loopback test

**Files:**
- Modify: `src/daemon/p2p_manager.cpp:3050-3113` (the three `test_*` helpers)
- Modify: `tests/network/test_quic_session.cpp` (rewrite `LoopbackHandshakeAndOneEncryptedStreamPayload`)

- [ ] **Step 1: Update `test_configure_relay_quic_server`**

Currently it stamps a `QuicSession` onto the peer and clears buffers. Replace `p2p_manager.cpp:3050-3071` with:

```cpp
bool P2PManager::test_configure_relay_quic_server(
    const std::string& virtual_peer_key,
    const dinero::network::QuicSessionOptions& options) {
    std::shared_ptr<PeerInfo> peer;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(virtual_peer_key);
        if (it != connected_peers_.end()) peer = it->second;
    }
    if (!peer || !peer->via_relay) return false;

    peer->via_relay->encrypted_quic = true;
    peer->relay_quic_options = options;
    peer->relay_quic_session = std::make_shared<dinero::network::QuicSession>(
        [this, virtual_peer_key](std::vector<uint8_t> bytes) {
            std::shared_ptr<PeerInfo> p;
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = connected_peers_.find(virtual_peer_key);
                if (it != connected_peers_.end()) p = it->second;
            }
            if (p) send_relay_payload_to_virtual_peer(*p, bytes);
        });
    return true;
}
```

- [ ] **Step 2: Delete `test_drain_relay_quic_packets`**

It depended on `relay_quic_outbox_packets` (removed in Task 7). Remove the implementation and the declaration in `p2p_manager.h`. If any test calls it, replace those calls with: test provides its own writer at session-construction time that captures bytes into a test-local container.

- [ ] **Step 3: Update `test_relay_quic_handshake_ready`**

Replace `p2p_manager.cpp:3099-3113` with:

```cpp
bool P2PManager::test_relay_quic_handshake_ready(const std::string& virtual_peer_key) {
    std::shared_ptr<PeerInfo> peer;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = connected_peers_.find(virtual_peer_key);
        if (it != connected_peers_.end()) peer = it->second;
    }
    if (!peer || !peer->relay_quic_session) return false;
    return peer->relay_quic_session->handshake_ready();
}
```

- [ ] **Step 4: Rewrite ALL tests in `tests/network/test_quic_session.cpp`**

Enumerate them first:

```bash
grep -n "^TEST(" tests/network/test_quic_session.cpp
```

Migrate every one — not only `LoopbackHandshakeAndOneEncryptedStreamPayload`. Any test calling `client.DrainOutgoing(...)`, `server.ReceivePacket(...)`, `server.StartServerFromInitial(...)`, `client.HandleExpiry()`, `client.QueueStreamData(...)`, or `server.TakeReceivedStreamData()` directly is using the removed API and must be rewritten against the threaded API.

Reference rewrite for `LoopbackHandshakeAndOneEncryptedStreamPayload` (apply the same shape to every other test in the file — pair of loopback-wired sessions, `WaitHandshakeReady` instead of pump loop, `QueueOutgoingStream` to send, `ReadDecryptedStream` to receive):

```cpp
TEST(QuicSession, LoopbackHandshakeAndOneEncryptedStreamPayload) {
    const auto info = dinero::network::QuicTransport::CompileInfo();
    ASSERT_TRUE(info.ngtcp2_available);
    ASSERT_EQ(info.crypto_backend, "ossl");
    ASSERT_TRUE(info.crypto_available) << info.disabled_reason;

    using dinero::network::QuicSession;
    std::shared_ptr<QuicSession> client_session;
    std::shared_ptr<QuicSession> server_session;
    auto client_writer = [&server_session](std::vector<uint8_t> bytes) {
        if (server_session) server_session->EnqueueIncomingPacket(std::move(bytes));
    };
    auto server_writer = [&client_session](std::vector<uint8_t> bytes) {
        if (client_session) client_session->EnqueueIncomingPacket(std::move(bytes));
    };
    server_session = std::make_shared<QuicSession>(server_writer);
    client_session = std::make_shared<QuicSession>(client_writer);

    auto options = StressOptions();  // reuse helper; or inline cert/key
    const auto client_addr = Localhost(22001);
    const auto server_addr = Localhost(22002);

    ASSERT_TRUE(server_session->StartServer(server_addr, client_addr, options));
    ASSERT_TRUE(client_session->StartClient(client_addr, server_addr, options));

    auto cr = client_session->WaitHandshakeReady();
    auto sr = server_session->WaitHandshakeReady();
    ASSERT_EQ(cr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(sr.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(cr.get());
    ASSERT_TRUE(sr.get());

    const std::vector<uint8_t> payload = {0x64, 0x69, 0x6e, 0x65, 0x72, 0x6f,
                                          0x2d, 0x71, 0x75, 0x69, 0x63};
    client_session->QueueOutgoingStream(payload, true);

    std::vector<uint8_t> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < payload.size() &&
           std::chrono::steady_clock::now() < deadline) {
        auto chunk = server_session->ReadDecryptedStream(std::chrono::milliseconds(100));
        received.insert(received.end(), chunk.begin(), chunk.end());
    }
    EXPECT_EQ(received, payload);
}
```

(If `StressOptions` is private to the stress test, copy the body inline here instead.)

- [ ] **Step 5: Build everything**

```bash
cmake --build build-check 2>&1 | tail -20
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/daemon/p2p_manager.cpp include/daemon/p2p_manager.h tests/network/test_quic_session.cpp
git commit -m "$(cat <<'EOF'
test+p2p: migrate test helpers and loopback test to threaded QuicSession API

Test helpers test_configure_relay_quic_server and
test_relay_quic_handshake_ready use QuicSession's new construction
signature (OutboundWriter callback). test_drain_relay_quic_packets is
removed — tests that need to capture outgoing bytes should pass a
capturing writer at construction.

LoopbackHandshakeAndOneEncryptedStreamPayload rewritten to use
WaitHandshakeReady / QueueOutgoingStream / ReadDecryptedStream
instead of manual packet pumping.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Full test suite run

**Files:**
- Run only.

- [ ] **Step 1: Configure a clean build directory**

```bash
cmake -S . -B build-pr126 -DCMAKE_BUILD_TYPE=Release
cmake --build build-pr126 -j 2>&1 | tail -10
```

Expected: clean build of everything (dinerod, dinero-qt-deps, tests).

- [ ] **Step 2: Run the QUIC test suite**

```bash
ctest --test-dir build-pr126 -R "quic" --output-on-failure 2>&1 | tail -40
```

Expected: all QUIC-related tests pass. The stress test specifically must report 1000/1000 successes.

- [ ] **Step 3: Run the P2P test suite**

```bash
ctest --test-dir build-pr126 -R "p2p|relay" --output-on-failure 2>&1 | tail -40
```

Expected: all pass. If a test fails referencing `relay_quic_mutex`, `relay_quic_stream_buffer`, `relay_quic_outbox_packets`, or `drain_relay_quic_outgoing` — go back to Task 7/8 and update it.

- [ ] **Step 4: Run the full ctest suite**

```bash
ctest --test-dir build-pr126 --output-on-failure 2>&1 | tail -50
```

Expected: full suite green. The codebase has ~365 tests per memory; if your output is significantly under that count, something didn't register — check that the stress test got added to the right CMakeLists.txt scope.

- [ ] **Step 5: Repeat the stress test 10x to verify stability under repeated runs**

```bash
for i in $(seq 1 10); do
  result=$(./build-pr126/tests/network/test_quic_session_stress 2>&1 | grep -E "PASSED|FAILED")
  echo "run $i: $result"
done
```

Expected: 10x `[  PASSED  ] 1 test.`

- [ ] **Step 6: Commit (validation-only step, no code change)**

No commit — proceed to canary if green. If anything is red, fix it and re-run; do NOT proceed to canary with red tests.

---

## Task 10: Canary soak instructions

**Files:**
- None — operational checklist for the user to execute.

This task is not run by an agent — these are the steps for the human operator (or a subsequent session with fleet access) to validate the refactor on real hardware.

- [ ] **Step 1: Build the Linux dinerod for fleet deployment**

Use the existing build path (Dell Tower per memory). The `dinero-main` binary plus the four new commits builds to whatever path `mcp__ops__build_dinero` or the manual build script produces.

- [ ] **Step 2: Deploy to ONE fleet node first (recommend VA — it was the working relay in the original canary)**

Per `feedback_daemon_shutdown.md`: stop with SIGTERM / RPC stop, never hard-kill (corrupts ChainDB). Per existing fleet pattern:

```bash
ssh va "systemctl stop dinerod.service"
scp dinerod va:/root/Dinero-Coin/build/dinerod
ssh va "systemctl start dinerod.service"
ssh va "systemctl status dinerod.service"
```

Wait ~60s for catch-up. Verify the relay registry is still populated:

```bash
ssh va "tail -100 /var/log/dinerod.log | grep relayreg"
```

Expected: `relayreg: registered ...` entries for the Mac and other registered peers.

- [ ] **Step 3: Run the canary handshake from a fresh origin**

From the Dell test box (or wherever `mcp__ops__server_exec` reaches an unrelated node):

1. Start an outbound circuit via VA to the Mac (use existing test orchestrator command).
2. Watch both ends:
   - On VA: expect `relay-orchestrator: opened circuit ...` 
   - On Mac: expect `relay-data: created inbound virtual peer ... (QUIC-encrypted)`, then `relay-handshake: QUIC handshake ready ...`, then normal dineroid identity exchange.
3. Run `dinero-cli getpeerinfo` on the Mac — expect a `relay:in:` peer.

Success criterion: `relay:in:` peer appears in Mac's `getpeerinfo` within 10 seconds of circuit opening.

- [ ] **Step 4: Sustain test — 100 sequential circuit cycles**

Loop:
- Open circuit
- Verify peer appears
- Close circuit
- Wait 5s
- Repeat 100×

If any cycle fails to complete the QUIC handshake in 10s, capture full log context (both ends, last 200 lines) and STOP. The refactor has not closed the bug class.

If all 100 cycles succeed → proceed to Step 5.

- [ ] **Step 5: Deploy to all 4 fleet nodes**

Same procedure as Step 2, applied to LA, MO, CN, and confirmed again on VA.

- [ ] **Step 6: 24-hour soak**

Leave the fleet running for 24 hours under whatever normal load exists. Monitor:
- `getnetworkinfo.connections_in` on the Mac — should be > 0 and stable.
- No new `QUIC handshake did not become ready` log entries on any fleet node.
- No crashes / restarts on any fleet node.
- Memory usage per `relay_active` peer doesn't grow unbounded (smoke check for queue accumulation).

If clean → PR #126 is ship-ready. If any failure mode appears → file a follow-up with full reproduction context. Do NOT add mutex band-aids on top — the whole point of this refactor was to eliminate that pattern.

- [ ] **Step 7: Open the PR**

```bash
cd /Users/haydarevich/src/dinero-v8
git push origin fix/quic-session-single-thread-ownership
gh pr create --title "QuicSession single-thread ownership (closes the v1-v4 race class)" --body "$(cat <<'EOF'
## Summary
- Replaces the multi-threaded ngtcp2 access pattern in QuicSession with a dedicated owning thread + queues
- Closes the bug class that defeated band-aid attempts v1-v4 on `fix/quic-handshake-race`
- Adds a 1000-iteration loopback stress test as the structural gate

## Architecture
QuicSession constructor starts an exclusive owner thread. Listen path enqueues incoming packets; send path enqueues outgoing stream data; handler thread waits on a `shared_future<bool>` for handshake completion. PeerInfo loses `relay_quic_mutex`, `relay_quic_stream_buffer`, `relay_quic_outbox_packets`.

## Test plan
- [x] 1000-iteration loopback stress test (run 10x, all green)
- [x] Full ctest suite green
- [x] Canary handshake on VA: relay:in: peer appears on Mac within 10s
- [x] 100 sequential circuit cycles all complete
- [x] 4-node fleet 24h soak: no handshake failures, stable connections_in

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review

### Spec coverage check

| Spec requirement (from conversation + commit messages) | Task |
|---|---|
| Single-thread ownership of ngtcp2 state | Task 3 |
| Eliminate `peer.relay_quic_mutex` external sync | Task 7 |
| Replace polling with future-wait | Task 6 |
| 10s handshake timeout preserved | Task 6 step 1 |
| Listen path becomes non-blocking enqueue | Task 5 |
| Send path becomes non-blocking enqueue | Task 7 |
| Decrypted-stream extraction off the listen thread | Task 5 step 4 + Task 6 step 1 |
| 1000x stress test as gate | Task 1 + Task 9 step 5 |
| Existing loopback test migrated | Task 8 step 4 |
| Non-QUIC paths unaffected | Task 7 (else-branch preserved verbatim) |
| Branch from `dinero-main`, not band-aid branch | Task 1 step 1 |
| Canary deployment procedure | Task 10 |

### Type consistency check

| Symbol | Declared in | Used in |
|---|---|---|
| `OutboundWriter` | Task 2 (header) | Task 3 (constructor body), Task 5 (relay listen-path construction), Task 8 (test helper construction) |
| `EnqueueIncomingPacket(std::vector<uint8_t>)` | Task 2 | Tasks 1, 5, 8 — consistent signature |
| `QueueOutgoingStream(std::vector<uint8_t>, bool fin)` | Task 2 | Task 7 (passes `framed, false`), Task 8 (passes `payload, true`) — consistent |
| `WaitHandshakeReady()` returns `shared_future<bool>` | Task 2 | Tasks 1, 6, 8 — all use `wait_for` + `get()` correctly |
| `ReadDecryptedStream(milliseconds)` | Task 2 | Task 5 step 4 (`run_relay_quic_reader_loop`), Task 8 (loopback test) — consistent |
| `Close()` | Task 2 | Task 3 (destructor), not called elsewhere in production code (intentional — destruction triggers close) |
| `run_relay_quic_reader_loop` | Task 5 (added to header) | Task 6 (spawned by handler thread) — consistent |

### Known gaps the executing agent will discover

1. **`CurrentNgtcpTimestampNs` is a placeholder name** for the existing "now in ngtcp2 nanoseconds" helper in `quic_session.cpp`. Find it via `grep -n "ngtcp2_tstamp\|now_ns\|CurrentNgtcp" src/network/quic_session.cpp` — it's the function whose return value is currently passed as the timestamp argument to `ngtcp2_conn_handle_expiry` and `ngtcp2_conn_read_pkt`. Use the same function in `NgtcpExpiryToSteady`. Getting the wrong clock domain here silently breaks retransmit timing (the v3 failure shape).
2. **`TakeReceivedStreamDataInternal` is a placeholder name** for the existing logic of `TakeReceivedStreamData` (currently public). Inline its body into the new internal helper of the same purpose.
3. **The exact CMakeLists.txt path for test registration** is not pinned in this plan. Step 3 of Task 1 specifies the grep command to find it.
4. **The client-side `StartClient` invocation site** in `p2p_manager.cpp` (mentioned in Task 5 Step 2) needs the writer wired up too. If the codebase only initiates outbound QUIC relay sessions in one place, that's the only edit needed; if there are multiple, all need the writer.
5. **`PeerInfo` is shared across all peer types**; removing the QUIC-specific fields shouldn't affect non-QUIC peers, but verify by building `dinerod` clean (Task 7 step 6).
6. **Per-QUIC-peer thread count increases from 1 to 3** (handler + session-owner + decrypted-stream reader). Trivial at current fleet scale (~20 extra threads), but worth noting if QUIC ever expands to all peers (design goal: "every dinero-qt is a real P2P node"). At N=1000 peers that's 3000 threads — fine on modern Linux but a number to know.
7. **`run_relay_quic_reader_loop` shutdown latency:** the loop blocks up to 200ms in `ReadDecryptedStream` before re-checking `shutdown_requested_` / `peer->is_connected`. Acceptable; documented here so a future debugger looking at "why is shutdown slow" finds the answer.

### Execution choice handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-22-quic-session-single-thread-ownership.md`. Two execution options:

**1. Subagent-Driven (recommended)** — Dispatch a fresh subagent per task, review between tasks, fast iteration. Recommended here because Tasks 3 and 5 each touch >300 lines and benefit from focused context per task.

**2. Inline Execution** — Execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints at task boundaries.

Which approach?
