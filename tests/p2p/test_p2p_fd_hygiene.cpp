/**
 * #373: P2P stale-fd double-close.
 *
 * cleanup_peer closed a peer's socket but never invalidated the stored
 * socket_fd, while the peer object stays in connected_peers_ until teardown.
 * Any second closer — another cleanup_peer for the same (churning) peer, or
 * stop()'s "idempotent" close loop — re-closed the STALE fd number, which by
 * then the kernel may have reused for anything else in the process. On EU1
 * (2026-07-04) the victim was RocksDB's in-flight sst file: EBADF mid-flush →
 * latched background error → 3-hour node zombie (#371/#372).
 *
 * These tests install a real-fd peer, clean it up, deliberately occupy the
 * freed fd number with a bystander file (POSIX: open() returns the lowest
 * free descriptor), run the second closer, and require the bystander to
 * survive. Pre-fix, the second close kills the bystander.
 */

#include "daemon/p2p_manager.h"

#include <gtest/gtest.h>

#include <array>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>



namespace {

// Occupy the just-freed descriptor number `target`: open /dev/null until the
// kernel hands us exactly that number (lowest-free guarantee), closing any
// lower fds we collected on the way. Returns the bystander fd (== target).
int OccupyFdNumber(int target) {
    std::vector<int> extras;
    int bystander = -1;
    for (int i = 0; i < 256; ++i) {
        int fd = ::open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        if (fd == target) {
            bystander = fd;
            break;
        }
        if (fd > target) {
            // Passed the target without hitting it — target is still open,
            // i.e. cleanup did not actually close it. Not this test's defect.
            ::close(fd);
            break;
        }
        extras.push_back(fd);
    }
    for (int fd : extras) ::close(fd);
    return bystander;
}

int InstallRealFdPeer(P2PManager& manager, const std::string& addr, int* other_end) {
    int sp[2] = {-1, -1};
    EXPECT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sp));
    *other_end = sp[1];
    manager.test_install_connected_direct_peer(
        addr, sp[0], /*is_outbound=*/true, /*identity_proven=*/false,
        std::array<uint8_t, 20>{});
    return sp[0];
}

TEST(P2PFdHygiene, CleanupPeerInvalidatesStoredFd) {
    P2PManager manager(0);
    int other_end = -1;
    const std::string addr = "203.0.113.7:20999";
    const int peer_fd = InstallRealFdPeer(manager, addr, &other_end);
    ASSERT_GE(peer_fd, 0);

    ASSERT_EQ(manager.test_peer_socket_fd(addr), peer_fd);
    manager.test_cleanup_peer(addr);

    // The close must happen (fd released)…
    EXPECT_EQ(::fcntl(peer_fd, F_GETFD), -1) << "cleanup_peer must close the socket";
    // …and the stored fd must be invalidated so no second closer can re-take it.
    EXPECT_EQ(manager.test_peer_socket_fd(addr), -1)
        << "cleanup_peer must set socket_fd = -1 after closing (#373)";

    ::close(other_end);
}

TEST(P2PFdHygiene, SecondCleanupSparesReusedFd) {
    P2PManager manager(0);
    int other_end = -1;
    const std::string addr = "203.0.113.8:20999";
    const int peer_fd = InstallRealFdPeer(manager, addr, &other_end);
    ASSERT_GE(peer_fd, 0);

    manager.test_cleanup_peer(addr);  // first cleanup closes the socket

    // Kernel reuses the freed number — the bystander now owns it (this is
    // rocksdb's sst fd in the EU1 incident).
    const int bystander = OccupyFdNumber(peer_fd);
    ASSERT_EQ(bystander, peer_fd) << "fd-reuse precondition not met";

    manager.test_cleanup_peer(addr);  // second cleanup — the EU1 killer path

    EXPECT_EQ(::fcntl(bystander, F_GETFD), 0)
        << "second cleanup_peer closed a reused fd it no longer owns (#373)";

    ::close(bystander);
    ::close(other_end);
}

TEST(P2PFdHygiene, StopSparesFdAlreadyClosedByCleanup) {
    P2PManager manager(0);
    int other_end = -1;
    const std::string addr = "203.0.113.9:20999";
    const int peer_fd = InstallRealFdPeer(manager, addr, &other_end);
    ASSERT_GE(peer_fd, 0);

    manager.test_cleanup_peer(addr);

    const int bystander = OccupyFdNumber(peer_fd);
    ASSERT_EQ(bystander, peer_fd) << "fd-reuse precondition not met";

    // stop()'s close loop claimed to be "idempotent" — it is not once the
    // number is reused. It must skip fds already invalidated by cleanup_peer.
    // (stop() early-returns unless running_; flip it so the loop actually runs.)
    manager.test_set_running(true);
    manager.stop();

    EXPECT_EQ(::fcntl(bystander, F_GETFD), 0)
        << "stop() closed a reused fd already closed by cleanup_peer (#373)";

    ::close(bystander);
    ::close(other_end);
}

}  // namespace
