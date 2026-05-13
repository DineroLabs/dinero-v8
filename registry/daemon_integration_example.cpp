// Example: Integrating Global Registry sync into dinerod
// Add this to src/httprpc.cpp or create new src/registry.cpp

#include <string>
#include <thread>
#include <chrono>
#include <json/json.h>
#include <curl/curl.h>
#include "util.h"
#include "logging.h"

// Configuration
static const int REGISTRY_SYNC_INTERVAL = 600; // 10 minutes
static std::thread g_registryThread;
static bool g_registryRunning = false;

// CURL callback for response
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// HTTP POST helper
static bool HttpPostJson(const std::string& url, const std::string& payload, std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        LogPrintf("Registry: Failed to init CURL\n");
        return false;
    }

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LogPrintf("Registry: POST failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    if (http_code != 200 && http_code != 201) {
        LogPrintf("Registry: HTTP %ld - %s\n", http_code, response);
        return false;
    }

    return true;
}

// Get server info (reuse existing GetServerInfoJson if available)
static Json::Value GetRegistrationPayload() {
    Json::Value payload;

    // Use external IP or configured address
    std::string publicIP = gArgs.GetArg("-externalip", "127.0.0.1");
    int rpcPort = gArgs.GetArg("-rpcport", 21999);

    payload["ip"] = publicIP;
    payload["rpc_port"] = rpcPort;
    payload["serverinfo_url"] = strprintf("http://%s:%d/serverinfo.json", publicIP, rpcPort);

    // Optional: Add node metadata
    payload["name"] = gArgs.GetArg("-nodename", "Dinero Node");
    payload["network"] = Params().NetworkIDString();

    return payload;
}

// Sync to registry
static void SyncToRegistry() {
    std::string registryUrl = gArgs.GetArg("-registryurl",
        "https://status.dinero-coin.com/api/register");

    LogPrintf("Registry: Syncing to %s\n", registryUrl);

    Json::Value payload = GetRegistrationPayload();
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string payloadStr = Json::writeString(builder, payload);

    std::string response;
    if (HttpPostJson(registryUrl, payloadStr, response)) {
        LogPrintf("Registry: Successfully registered - %s\n", response);
    } else {
        LogPrintf("Registry: Registration failed\n");
    }
}

// Background thread
static void RegistryThreadHandler() {
    LogPrintf("Registry: Sync thread started\n");

    while (g_registryRunning) {
        try {
            SyncToRegistry();
        } catch (const std::exception& e) {
            LogPrintf("Registry: Exception: %s\n", e.what());
        }

        // Sleep with interrupt check
        for (int i = 0; i < REGISTRY_SYNC_INTERVAL && g_registryRunning; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LogPrintf("Registry: Sync thread stopped\n");
}

// Start registry sync
void StartRegistrySync() {
    if (!gArgs.GetBoolArg("-registry", false)) {
        LogPrintf("Registry: Disabled (-registry=0)\n");
        return;
    }

    g_registryRunning = true;
    g_registryThread = std::thread(&RegistryThreadHandler);

    LogPrintf("Registry: Enabled, will sync every %d seconds\n", REGISTRY_SYNC_INTERVAL);
}

// Stop registry sync
void StopRegistrySync() {
    if (g_registryRunning) {
        g_registryRunning = false;
        if (g_registryThread.joinable()) {
            g_registryThread.join();
        }
    }
}

// Usage in src/init.cpp:
//
// void InitStartup() {
//     ...
//     StartRegistrySync();
//     ...
// }
//
// void Shutdown() {
//     ...
//     StopRegistrySync();
//     ...
// }
//
// dinero.conf:
//   registry=1
//   registryurl=https://status.dinero-coin.com/api/register
//   externalip=173.249.195.59
//   nodename=Virginia
