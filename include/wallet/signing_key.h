#pragma once
#include <openssl/crypto.h>
#include <utility>
#include <vector>
#include <cstdint>

namespace dinero {
// Policy travels with the internal scalar and exact consumed script. It is not
// inferred from an address prefix, shortened label, or attempted signature.
enum class SigningKeyPolicy { Untweaked, TaprootCanonical, TaprootHistoricalImport };
struct SigningKey {
    std::vector<uint8_t> script;
    std::vector<uint8_t> secret;
    SigningKeyPolicy policy=SigningKeyPolicy::Untweaked;
    SigningKey()=default;
    SigningKey(std::vector<uint8_t> key,std::vector<uint8_t> spk,SigningKeyPolicy kind)
        :script(std::move(spk)),secret(std::move(key)),policy(kind){}
    SigningKey(const SigningKey& other):script(other.script),secret(other.secret),policy(other.policy){}
    SigningKey(SigningKey&& other) noexcept
        :script(std::move(other.script)),secret(std::move(other.secret)),policy(other.policy){}
    SigningKey& operator=(SigningKey other) noexcept {
        secret.swap(other.secret);script.swap(other.script);std::swap(policy,other.policy);return *this;
    }
    ~SigningKey(){if(!secret.empty())OPENSSL_cleanse(secret.data(),secret.size());}
};
}
