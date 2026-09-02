#include <jni.h>

#include "nodecore/nodecore_ffi.h"
#include "wallet/bip39.h"
#include "wallet/bip32_deriver.h"
#include "wallet/shielded_derivation.h"

#include <json/json.h>
#include <openssl/crypto.h>
#include <sstream>
#include <vector>

namespace {

const char* GetUtf(JNIEnv* env, jstring value) {
    return value == nullptr ? nullptr : env->GetStringUTFChars(value, nullptr);
}

void ReleaseUtf(JNIEnv* env, jstring value, const char* chars) {
    if (value != nullptr && chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeP2mrAddresses(
    JNIEnv* env, jobject, jstring mnemonic, jstring passphrase,
    jint count, jboolean is_change) {
    const char* mnemonic_chars = GetUtf(env, mnemonic);
    const char* passphrase_chars = GetUtf(env, passphrase);
    if (mnemonic_chars == nullptr || passphrase_chars == nullptr || count < 1 || count > 100) {
        ReleaseUtf(env, passphrase, passphrase_chars);
        ReleaseUtf(env, mnemonic, mnemonic_chars);
        return nullptr;
    }
    std::vector<uint8_t> seed;
    std::ostringstream addresses;
    try {
        if (!dinero::bip39::MnemonicToSeed(mnemonic_chars, passphrase_chars, seed) || seed.size() != 64) {
            throw std::runtime_error("invalid mnemonic");
        }
        for (int index = 0; index < count; ++index) {
            dinero::BIP32Deriver child(seed.data(), seed.size());
            child.deriveHardened(88);
            child.deriveHardened(1448);
            child.deriveHardened(0);
            child.deriveNormal(is_change ? 1 : 0);
            child.deriveNormal(static_cast<uint32_t>(index));
            auto priv = child.getPrivateKey();
            auto chain = child.getChainCode();
            char* json = nodecore_p2mr_derive_address_json(
                "din", priv.data(), priv.size(), chain.data(), chain.size(), 0);
            OPENSSL_cleanse(priv.data(), priv.size());
            OPENSSL_cleanse(chain.data(), chain.size());
            if (json == nullptr) throw std::runtime_error("P2MR derivation failed");
            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream input(json);
            const bool parsed = Json::parseFromStream(builder, input, &root, &errors);
            nodecore_free_string(json);
            if (!parsed || !root["ok"].asBool() || root["address"].asString().empty()) {
                throw std::runtime_error("P2MR derivation failed");
            }
            if (index != 0) addresses << '\n';
            addresses << root["address"].asString();
        }
    } catch (...) {
        if (!seed.empty()) OPENSSL_cleanse(seed.data(), seed.size());
        ReleaseUtf(env, passphrase, passphrase_chars);
        ReleaseUtf(env, mnemonic, mnemonic_chars);
        return nullptr;
    }
    OPENSSL_cleanse(seed.data(), seed.size());
    ReleaseUtf(env, passphrase, passphrase_chars);
    ReleaseUtf(env, mnemonic, mnemonic_chars);
    return env->NewStringUTF(addresses.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeShieldedAddresses(
    JNIEnv* env, jobject, jstring mnemonic, jstring passphrase, jint count) {
    const char* mnemonic_chars = GetUtf(env, mnemonic);
    const char* passphrase_chars = GetUtf(env, passphrase);
    if (mnemonic_chars == nullptr || passphrase_chars == nullptr || count < 1 || count > 100) {
        ReleaseUtf(env, passphrase, passphrase_chars);
        ReleaseUtf(env, mnemonic, mnemonic_chars);
        return nullptr;
    }
    std::vector<uint8_t> seed;
    std::ostringstream addresses;
    try {
        if (!dinero::bip39::MnemonicToSeed(mnemonic_chars, passphrase_chars, seed) || seed.size() != 64) {
            throw std::runtime_error("invalid mnemonic");
        }
        const auto keys = dinero::wallet::shielded::DeriveShieldedAccount(seed.data(), seed.size(), 0);
        for (int index = 0; index < count; ++index) {
            using namespace dinero::wallet::shielded;
            const auto diversifier = ChaCha20Diversifier(keys.dk, static_cast<uint64_t>(index));
            const auto point = HashToPoint(diversifier, kDstDiv);
            const auto pk_d = DerivePkD(keys.ivk, point);
            const auto spend = DeriveDiversifiedSpendKey(keys.ivk, diversifier).pk_d;
            const auto payload = BuildAddressPayload(diversifier, pk_d, spend);
            if (index != 0) addresses << '\n';
            addresses << EncodeShieldedAddress(payload, kHrpMainnet);
        }
    } catch (...) {
        if (!seed.empty()) OPENSSL_cleanse(seed.data(), seed.size());
        ReleaseUtf(env, passphrase, passphrase_chars);
        ReleaseUtf(env, mnemonic, mnemonic_chars);
        return nullptr;
    }
    OPENSSL_cleanse(seed.data(), seed.size());
    ReleaseUtf(env, passphrase, passphrase_chars);
    ReleaseUtf(env, mnemonic, mnemonic_chars);
    return env->NewStringUTF(addresses.str().c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeStart(
    JNIEnv* env, jobject, jstring datadir, jstring config_json) {
    const char* datadir_chars = GetUtf(env, datadir);
    const char* config_chars = GetUtf(env, config_json);
    if (datadir_chars == nullptr) {
        ReleaseUtf(env, config_json, config_chars);
        return NODECORE_ERROR_INVALID_ARGS;
    }
    const int32_t result = nodecore_start(datadir_chars, config_chars);
    ReleaseUtf(env, config_json, config_chars);
    ReleaseUtf(env, datadir, datadir_chars);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeRpcCall(
    JNIEnv* env, jobject, jstring method, jstring params_json) {
    const char* method_chars = GetUtf(env, method);
    const char* params_chars = GetUtf(env, params_json);
    if (method_chars == nullptr) {
        ReleaseUtf(env, params_json, params_chars);
        return nullptr;
    }
    char* response = nodecore_rpc_call(method_chars, params_chars);
    ReleaseUtf(env, params_json, params_chars);
    ReleaseUtf(env, method, method_chars);
    if (response == nullptr) return nullptr;
    jstring result = env->NewStringUTF(response);
    nodecore_free_string(response);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeStop(JNIEnv*, jobject) {
    return nodecore_stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeIsRunning(JNIEnv*, jobject) {
    return nodecore_is_running() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeStatusJson(JNIEnv* env, jobject) {
    char* status = nodecore_get_status_json();
    if (status == nullptr) return nullptr;
    jstring result = env->NewStringUTF(status);
    nodecore_free_string(status);
    return result;
}
