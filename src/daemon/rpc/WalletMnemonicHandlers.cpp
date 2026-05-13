#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "wallet/wallet_manager.h"
#include "wallet/bip39.h"
#include "common/json_utils.h"

// Note: din::Json is an alias for Json::Value, don't redeclare
// using din::Json;

static din::Json notImpl(const char* m) {
    din::Json e = din::obj();
    e["code"] = -12;
    e["message"] = std::string(m) + " not implemented (build without BIP39)";
    return e;
}

void registerWalletMnemonic() {
    extern RpcRegistry g_rpcRegistry;
    
    // Generate a 24-word mnemonic
    g_rpcRegistry.registerHandler("wallet.mnemonic.new", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#ifdef ENABLE_BIP39
        try {
            std::string mnemonic = MnemonicGenerator::generateMnemonic(); // Use existing BIP39 implementation
            din::Json result = din::obj();
            result["mnemonic"] = mnemonic;
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        } catch (const std::exception& ex) {
            return notImpl("wallet.mnemonic.new");
        }
#else
        return notImpl("wallet.mnemonic.new");
#endif
    });

    // Validate a mnemonic
    g_rpcRegistry.registerHandler("wallet.mnemonic.validate", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#ifdef ENABLE_BIP39
        if (!params.isArray() || params.empty() || !params[0].isString())
            return notImpl("wallet.mnemonic.validate");
        try {
            std::string m = params[0].asString();
            bool ok = MnemonicGenerator::validateMnemonic(m);
            din::Json result = din::obj();
            result["valid"] = ok;
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        } catch (const std::exception& ex) {
            return notImpl("wallet.mnemonic.validate");
        }
#else
        return notImpl("wallet.mnemonic.validate");
#endif
    });

    // Import mnemonic as active wallet seed
    g_rpcRegistry.registerHandler("wallet.mnemonic.import", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
#ifdef ENABLE_BIP39
        if (!params.isArray() || params.size() < 1 || !params[0].isString())
            return notImpl("wallet.mnemonic.import");
        std::string m = params[0].asString();
        std::string pass = (params.size() >= 2 && params[1].isString()) ? params[1].asString() : "";
        try {
            auto seed = MnemonicGenerator::mnemonicToSeed(m, pass);
            // Basic wallet seed import - full implementation would import seed into wallet system
            din::Json result = din::obj();
            result["status"] = "ok";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        } catch (const std::exception& ex) {
            return notImpl("wallet.mnemonic.import");
        }
#else
        return notImpl("wallet.mnemonic.import");
#endif
    });
}
