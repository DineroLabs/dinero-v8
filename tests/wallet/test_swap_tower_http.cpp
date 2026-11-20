// The public tower's door (tower_http.h): what it answers, and that the buyer's
// side arms only on an answer naming exactly its package. No sockets: the
// buyer's HTTP is wired straight to the tower's handler.
#include "wallet/swap/tower_http.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dinero::swap;

namespace {

constexpr uint32_t kNow = 1'800'000'000;

// The package of tests/wallet/data (a real, verified regtest package).
std::string GoldenPackage() {
    std::ifstream in(std::string(SWAP_TEST_DATA_DIR) + "/tower_package_golden.txt");
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

struct Tower {
    std::string dir;
    TowerIntakeLimits limits;
    uint32_t now{kNow};
    int posts{0}, gets{0};
    explicit Tower(const char* name) : dir((std::filesystem::temp_directory_path() / name).string()) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~Tower() { std::filesystem::remove_all(dir); }
    static std::string PathOf(const std::string& url) {
        const auto at = url.find('/', std::string("https://").size());
        return at == std::string::npos ? "/" : url.substr(at);
    }
    BtcHttp Http() {
        BtcHttp h;
        h.get = [this](const std::string& url) {
            ++gets;
            const auto r = HandleTowerHttpRequest("GET", PathOf(url), "", dir, limits, now);
            return BtcHttpResponse{r.status, r.body};
        };
        h.post = [this](const std::string& url, const std::string& body) {
            ++posts;
            const auto r = HandleTowerHttpRequest("POST", PathOf(url), body, dir, limits, now);
            return BtcHttpResponse{r.status, r.body};
        };
        return h;
    }
};

}  // namespace

TEST(SwapTowerHttp, TheGoldenPackageIsThere) { ASSERT_GT(GoldenPackage().size(), 1000u); }

TEST(SwapTowerHttp, ABuyerArmsATowerAndSeesItsAckOnlyWhileFresh) {
    Tower tower("swap_tower_http_arm");
    const std::string text = GoldenPackage(), hash = TowerPackageHash(text);
    const auto sink = MakeHttpTowerSink("https://tower.example/", tower.Http());
    const auto ack = MakeHttpTowerAck("https://tower.example", tower.Http());
    EXPECT_FALSE(ack("ignored", hash)) << "nothing sent yet";
    sink(text);
    EXPECT_TRUE(std::filesystem::exists(tower.dir + "/" + hash + ".pkg"));
    EXPECT_FALSE(ack("ignored", hash)) << "held, but the tower has not confirmed it is guarding it";
    MarkTowerArmed(tower.dir, hash, hash, kNow);  // what the tower's loop does while healthy
    tower.now = kNow + 30;
    EXPECT_TRUE(ack("ignored", hash));
    tower.now = kNow + 600;
    EXPECT_FALSE(ack("ignored", hash)) << "the tower stopped confirming";
    sink(text);  // sending it again is harmless
    EXPECT_EQ(tower.posts, 2);
}

TEST(SwapTowerHttp, TheSinkThrowsUnlessTheTowerHoldsExactlyThisPackage) {
    Tower tower("swap_tower_http_refuse");
    const std::string text = GoldenPackage();
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", tower.Http())("not a package"), std::runtime_error);
    tower.limits.max_packages = 0;
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", tower.Http())(text), std::runtime_error) << "full";
    BtcHttp silent;
    silent.post = [](const std::string&, const std::string&) { return BtcHttpResponse{}; };
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", silent)(text), std::runtime_error) << "no answer";
    BtcHttp yes_man;  // says yes without naming the package
    yes_man.post = [](const std::string&, const std::string&) { return BtcHttpResponse{200, "{\"hash\":\"00\"}"}; };
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", yes_man)(text), std::runtime_error);
    // The right hash under a refusal is still a refusal.
    const std::string named = "{\"hash\":\"" + TowerPackageHash(text) + "\"}";
    yes_man.post = [named](const std::string&, const std::string&) { return BtcHttpResponse{503, named}; };
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", yes_man)(text), std::runtime_error);
    yes_man.post = [named](const std::string&, const std::string&) { return BtcHttpResponse{200, named}; };
    EXPECT_NO_THROW(MakeHttpTowerSink("https://t.example", yes_man)(text));
    yes_man.post = [](const std::string&, const std::string&) { return BtcHttpResponse{200, "ok"}; };
    EXPECT_THROW(MakeHttpTowerSink("https://t.example", yes_man)(text), std::runtime_error);
}

TEST(SwapTowerHttp, AnAckIsOnlyAClearYes) {
    const std::string hash(64, 'a');
    auto answers = [&](int status, const std::string& body) {
        BtcHttp h;
        h.get = [=](const std::string&) { return BtcHttpResponse{status, body}; };
        return MakeHttpTowerAck("https://t.example", h)("id", hash);
    };
    EXPECT_TRUE(answers(200, "{\"armed\":true,\"age\":3}"));
    EXPECT_FALSE(answers(200, "{\"armed\":false}"));
    EXPECT_FALSE(answers(200, "{\"armed\":\"true\"}"));
    EXPECT_FALSE(answers(200, "true"));
    EXPECT_FALSE(answers(500, "{\"armed\":true}"));
    EXPECT_FALSE(answers(0, ""));
    int asked = 0;
    BtcHttp h;
    h.get = [&](const std::string&) { ++asked; return BtcHttpResponse{200, "{\"armed\":true}"}; };
    EXPECT_FALSE(MakeHttpTowerAck("https://t.example", h)("id", "../../etc/passwd"));
    EXPECT_EQ(asked, 0) << "only a hash is ever put in an address";
}

TEST(SwapTowerHttp, TheDoorAnswersOnlyWhatItOffers) {
    Tower tower("swap_tower_http_routes");
    auto ask = [&](const char* method, const std::string& target, const std::string& body = "") {
        return HandleTowerHttpRequest(method, target, body, tower.dir, tower.limits, kNow);
    };
    EXPECT_EQ(ask("GET", "/v1/health").status, 200);
    EXPECT_EQ(ask("GET", "/").status, 404);
    EXPECT_EQ(ask("GET", "/v1/packages").status, 405);
    EXPECT_EQ(ask("POST", "/v1/armed/" + std::string(64, 'a')).status, 405);
    EXPECT_EQ(ask("GET", "/v1/armed/" + std::string(64, 'a')).body, "{\"armed\":false}");
    EXPECT_EQ(ask("GET", "/v1/armed/../../x").status, 400);
    EXPECT_EQ(ask("GET", "/v1/armed/" + std::string(64, 'A')).status, 400);
    EXPECT_EQ(ask("POST", "/v1/packages", "junk").status, 400);
    tower.limits.max_package_bytes = 10;
    EXPECT_EQ(ask("POST", "/v1/packages", GoldenPackage()).status, 413);
    EXPECT_TRUE(std::filesystem::is_empty(tower.dir));
    // A store that cannot be written is the tower's problem, not told to strangers.
    const auto gone = HandleTowerHttpRequest("POST", "/v1/packages", GoldenPackage(), tower.dir + "/missing", {}, kNow);
    EXPECT_EQ(gone.status, 500);
    EXPECT_EQ(gone.body.find("missing"), std::string::npos);
}

TEST(SwapTowerHttp, OnlyAnHttpsAddressOrThisMachine) {
    EXPECT_EQ(TowerUrlProblem("https://tower.dinerolabs.org"), "");
    EXPECT_EQ(TowerUrlProblem("https://tower.dinerolabs.org:8443/swap"), "");
    EXPECT_EQ(TowerUrlProblem("http://127.0.0.1:8080"), "");
    EXPECT_EQ(TowerUrlProblem("http://localhost:8080"), "");
    EXPECT_NE(TowerUrlProblem("http://tower.dinerolabs.org"), "");
    EXPECT_NE(TowerUrlProblem("http://127.0.0.1.evil.example"), "");
    EXPECT_NE(TowerUrlProblem("tower.dinerolabs.org"), "");
    EXPECT_NE(TowerUrlProblem("https://"), "");
    EXPECT_NE(TowerUrlProblem("https://user@tower.example"), "");
    EXPECT_NE(TowerUrlProblem("https://tower.example/?x=1"), "");
    EXPECT_NE(TowerUrlProblem("https://tower.example/a b"), "");
}
