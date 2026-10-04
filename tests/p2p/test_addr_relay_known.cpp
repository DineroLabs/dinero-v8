// An address the node already knows must not be saved or relayed again.
//
// remember_peer_address() reported every address as new, so each addr/addrv2
// message rewrote peers.dat and was relayed to two outbound peers. Fleet nodes
// bounced the same few addresses back and forth (~65 addrv2/s observed on TX,
// 2026-10-03); the queued gossip held up block replies on every peer thread.
#include "p2p_manager.h"
#include "p2p/addr_v2.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>

namespace {
P2PMessage AddrFor(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    dinero::p2p::AddrV2Entry entry{};
    entry.net = dinero::p2p::NetworkType::IPV4;
    entry.addr = {a, b, c, d};
    entry.port = 20999;
    return P2PMessage::create_addrv2({entry});
}

std::filesystem::path FreshPeersFile() {
    auto path = std::filesystem::temp_directory_path() /
                ("addr-relay-known-" + std::to_string(::getpid()) + ".dat");
    std::filesystem::remove(path);
    return path;
}
}  // namespace

TEST(AddrRelayKnown, KnownAddressIsNotSavedAgain) {
    const auto peers_file = FreshPeersFile();
    P2PManager manager(20999);
    manager.load_peers(peers_file.string());  // no file yet: just sets the path

    manager.handle_addrv2("198.51.100.7:40000", AddrFor(8, 8, 4, 4));
    ASSERT_TRUE(std::filesystem::exists(peers_file)) << "a new address must be saved";

    std::filesystem::remove(peers_file);
    manager.handle_addrv2("198.51.100.7:40000", AddrFor(8, 8, 4, 4));
    EXPECT_FALSE(std::filesystem::exists(peers_file))
        << "the same address again must not count as new";

    manager.handle_addrv2("198.51.100.7:40000", AddrFor(1, 1, 1, 1));
    EXPECT_TRUE(std::filesystem::exists(peers_file)) << "a different address is still new";
    std::filesystem::remove(peers_file);
}
