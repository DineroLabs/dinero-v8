#include "network/tor_control.h"
#include "network/tor_runtime_controller.h"
#include "p2p/addr_v2.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
class FakeOnionService final : public dinero::network::ITorOnionService {
public:
    bool start_result{true};
    bool stop_result{true};
    dinero::network::TorOnionServiceStatus state;
    bool Start() override {
        state.requested = true;
        state.active = start_result;
        if (start_result) state.onion_address = "runtimecontroltest.onion";
        return start_result;
    }
    bool Stop() override {
        if (stop_result) state.active = false;
        return stop_result;
    }
    const dinero::network::TorOnionServiceStatus& status() const override {
        return state;
    }
};

std::string TempPath(const std::string& leaf) {
    static unsigned counter = 0;
    return (std::filesystem::temp_directory_path() /
            ("dinero-tor-runtime-" + leaf + "-" + std::to_string(++counter))).string();
}
}  // namespace

TEST(TorControlReply, ParsesMultilineSuccess) {
    const std::string wire =
        "250-PROTOCOLINFO 1\r\n"
        "250-AUTH METHODS=COOKIE COOKIEFILE=\"/tmp/control_auth_cookie\"\r\n"
        "250 OK\r\n";
    dinero::network::TorControlReply reply;
    std::string error;
    ASSERT_TRUE(dinero::network::ParseTorControlReply(wire, &reply, &error))
        << error;
    EXPECT_EQ(reply.code, 250);
    ASSERT_EQ(reply.lines.size(), 3u);
    EXPECT_EQ(reply.lines[1],
              "AUTH METHODS=COOKIE COOKIEFILE=\"/tmp/control_auth_cookie\"");
}

TEST(TorControlReply, RejectsIncompleteOrMixedReply) {
    dinero::network::TorControlReply reply;
    std::string error;
    EXPECT_FALSE(dinero::network::ParseTorControlReply(
        "250-PROTOCOLINFO 1\r\n", &reply, &error));
    EXPECT_FALSE(dinero::network::ParseTorControlReply(
        "250-one\r\n551 two\r\n", &reply, &error));
}

TEST(TorV3Address, CanonicalRoundTripAndChecksumValidation) {
    std::vector<uint8_t> public_key(32);
    for (size_t i = 0; i < public_key.size(); ++i) {
        public_key[i] = static_cast<uint8_t>(i);
    }
    std::string onion;
    std::string error;
    ASSERT_TRUE(dinero::p2p::EncodeTorV3Address(public_key, &onion, &error))
        << error;
    EXPECT_EQ(onion.size(), 62u);
    EXPECT_EQ(onion.substr(56), ".onion");

    std::vector<uint8_t> decoded;
    ASSERT_TRUE(dinero::p2p::DecodeTorV3Address(onion, &decoded, &error))
        << error;
    EXPECT_EQ(decoded, public_key);

    onion[55] = onion[55] == 'a' ? 'b' : 'a';
    EXPECT_FALSE(dinero::p2p::DecodeTorV3Address(onion, &decoded, &error));
}

TEST(TorV3Address, Addrv2CarriesOnlyPublicKey) {
    std::vector<uint8_t> public_key(32, 0x5a);
    dinero::p2p::AddrV2Entry entry;
    entry.time = 42;
    entry.services = 1;
    entry.net = dinero::p2p::NetworkType::TORV3;
    entry.addr = public_key;
    entry.port = 20999;

    const auto wire = dinero::p2p::EncodeAddrV2({entry});
    std::vector<dinero::p2p::AddrV2Entry> decoded;
    std::string error;
    ASSERT_TRUE(dinero::p2p::DecodeAddrV2(wire, &decoded, &error)) << error;
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded.front().net, dinero::p2p::NetworkType::TORV3);
    EXPECT_EQ(decoded.front().addr, public_key);
    EXPECT_EQ(decoded.front().port, 20999);
}

TEST(TorRuntimeController, TorAbsentPersistsOptInWithoutLeakingError) {
    const std::string pref = TempPath("absent");
    std::filesystem::remove(pref);
    dinero::network::TorRuntimeController controller(
        dinero::network::TorOptInStore(pref),
        [] { auto fake = std::make_unique<FakeOnionService>();
             fake->start_result = false; return fake; }, {}, {});
    const auto status = controller.SetEnabled(true);
    EXPECT_TRUE(status.requested);
    EXPECT_FALSE(status.active);
    EXPECT_EQ(dinero::network::TorOptInStore(pref).Read(), true);
    EXPECT_EQ(status.message,
              "Tor onion reachability is unavailable; check the local daemon log");
    std::ifstream stored(pref, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stored), {}), "1\n");
    std::filesystem::remove(pref);
}

TEST(TorRuntimeController, DisablePreservesStableOnionIdentity) {
    const std::string pref = TempPath("identity-pref");
    const std::string key_path = TempPath("identity-key");
    std::filesystem::remove(pref);
    std::filesystem::remove(key_path);
    const std::string key = "ED25519-V3:stable-private-identity";
    std::string error;
    ASSERT_TRUE(dinero::network::WriteTorPrivateKey(key_path, key, &error));
    dinero::network::TorRuntimeController controller(
        dinero::network::TorOptInStore(pref),
        [] { return std::make_unique<FakeOnionService>(); }, {}, {});
    ASSERT_TRUE(controller.SetEnabled(true).active);
    ASSERT_FALSE(controller.SetEnabled(false).active);
    EXPECT_EQ(dinero::network::ReadTorPrivateKey(key_path), key);
    std::filesystem::remove(pref);
    std::filesystem::remove(key_path);
}

TEST(TorRuntimeController, ActivationDisableAndRestartPersistence) {
    const std::string pref = TempPath("restart");
    std::filesystem::remove(pref);
    std::vector<std::string> advertised;
    auto factory = [] { return std::make_unique<FakeOnionService>(); };
    {
        dinero::network::TorRuntimeController controller(
            dinero::network::TorOptInStore(pref), factory,
            [&](const std::string& a) { advertised.push_back(a); }, {});
        EXPECT_TRUE(controller.SetEnabled(true).active);
        ASSERT_EQ(advertised.size(), 1u);
    }
    {
        dinero::network::TorRuntimeController restarted(
            dinero::network::TorOptInStore(pref), factory, {}, {});
        EXPECT_TRUE(restarted.Initialize(false).active);
        const auto off = restarted.SetEnabled(false);
        EXPECT_FALSE(off.requested);
        EXPECT_FALSE(off.active);
        EXPECT_EQ(dinero::network::TorOptInStore(pref).Read(), false);
    }
#ifndef _WIN32
    struct stat st {};
    ASSERT_EQ(::stat(pref.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);
#endif
    std::filesystem::remove(pref);
}

TEST(TorRuntimeController, FailedDisableStaysActiveAndDoesNotPersistOff) {
    const std::string pref = TempPath("disable-fail");
    std::filesystem::remove(pref);
    FakeOnionService* raw = nullptr;
    dinero::network::TorRuntimeController controller(
        dinero::network::TorOptInStore(pref),
        [&] { auto fake = std::make_unique<FakeOnionService>();
              raw = fake.get(); return fake; }, {}, {});
    ASSERT_TRUE(controller.SetEnabled(true).active);
    raw->stop_result = false;
    const auto status = controller.SetEnabled(false);
    EXPECT_TRUE(status.requested);
    EXPECT_TRUE(status.active);
    EXPECT_EQ(dinero::network::TorOptInStore(pref).Read(), true);
    std::filesystem::remove(pref);
}

TEST(TorPrivateKey, AtomicStorageIsOwnerOnlyAndRoundTrips) {
    const std::string path = TempPath("private-key");
    std::filesystem::remove(path);
    const std::string key = "ED25519-V3:deterministic-private-key-material";
    std::string error;
    ASSERT_TRUE(dinero::network::WriteTorPrivateKey(path, key, &error)) << error;
    EXPECT_EQ(dinero::network::ReadTorPrivateKey(path), key);
#ifndef _WIN32
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);
#endif
    std::filesystem::remove(path);
}
