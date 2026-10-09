#pragma once
#include <cstdint>
#include <string>
// Test-only ABI boundary: node fixtures never include Qt headers or macros.
namespace OrchardBackendContractTest {
struct Call { std::string method, params; };
Call Account(const std::string& method, uint64_t account);
Call Received(uint64_t account, uint64_t offset=0, uint64_t revision=0);
Call Payment(bool shield, bool withdraw, uint64_t account, uint64_t revision,
             const std::string& id, const std::string& address, uint64_t amount, uint64_t fee);
Call Finish(bool shield, uint64_t account, const std::string& id);
// Serializes only fields obtained from the actual production Qt parser.
std::string Parse(const std::string& kind, const std::string& wire,
                  const std::string& context="");
}
