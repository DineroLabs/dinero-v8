#include "orchard_backend.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <type_traits>
#include <vector>
using namespace dinero::orchard;
static_assert(!std::is_default_constructible_v<ParsedBundle>);
static_assert(!std::is_default_constructible_v<VerifiedAuthorization>);
static_assert(!std::is_assignable_v<decltype(std::declval<ParsedBundle>().UnverifiedFacts()),
                                   DineroOrchardFacts>);
static void Require(bool ok) { if (!ok) throw std::runtime_error("test check failed"); }
static auto Load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("missing fixture: " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        const std::string base = argv[1];
        auto bytes = Load(base + "/candidate-spend.bundle");
        const auto digest_bytes = Load(base + "/candidate-spend.digest");
        Require(digest_bytes.size() == 32);
        Hash digest{};
        std::copy(digest_bytes.begin(), digest_bytes.end(), digest.begin());
        auto parsed = ParsedBundle::Decode(bytes);
        const auto balance = parsed.UnverifiedFacts().value_balance;
        const auto count = parsed.UnverifiedFacts().action_count;
        Require(count == 2);
        // Ownership is independent of the original buffer and parsed wrapper.
        std::fill(bytes.begin(), bytes.end(), 0);
        auto verified = [&] {
            auto temporary = parsed;
            return temporary.VerifyAuthorization(digest, balance);
        }();
        Require(verified.SigningDigest() == digest);
        Require(verified.Facts().action_count == count);
        bool rejected = false;
        try { (void)parsed.VerifyAuthorization(digest, balance + 1); }
        catch (const BackendError& e) { rejected = e.Status() == 13; }
        Require(rejected);
        digest[0] ^= 1;
        rejected = false;
        try { (void)parsed.VerifyAuthorization(digest, balance); }
        catch (const BackendError& e) { rejected = e.Status() == 7; }
        Require(rejected);
        rejected = false;
        try { (void)ParsedBundle::Decode(bytes); }
        catch (const BackendError&) { rejected = true; }
        Require(rejected);
        std::cout << "Orchard C++ ownership and authorization checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
