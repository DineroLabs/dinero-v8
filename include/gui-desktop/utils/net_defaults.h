#pragma once
#include <unordered_map>
#include <string>
#include <QMetaType>

// Forward declare daemon Network enum for conversion
namespace din { enum class Network; }

// GUI-side network enum
enum class Network {
    Mainnet,
    Testnet,
    Regtest
};
Q_DECLARE_METATYPE(Network)

// Port configuration per network
struct NetPort { 
    int rpc; 
    int ws; 
};

// Fixed port mapping - no conflicts, no dynamic allocation
inline const std::unordered_map<Network, NetPort> NETWORK_PORTS = {
    {Network::Mainnet, {20998, 21001}},  // Production ports
    {Network::Testnet, {20988, 21011}},  // Testnet ports  
    {Network::Regtest, {20978, 21021}}   // Development ports
};

// Network name helpers
inline const char* networkName(Network n) {
    switch (n) {
        case Network::Mainnet: return "mainnet";
        case Network::Testnet: return "testnet"; 
        case Network::Regtest: return "regtest";
    }
    return "unknown";
}

inline std::string networkDisplayName(Network n) {
    switch (n) {
        case Network::Mainnet: return "Mainnet";
        case Network::Testnet: return "Testnet";
        case Network::Regtest: return "Regtest"; 
    }
    return "Unknown";
}

// Port accessors
inline NetPort portsFor(Network n) {
    auto it = NETWORK_PORTS.find(n);
    return (it != NETWORK_PORTS.end()) ? it->second : NetPort{0, 0};
}

inline int rpcPortFor(Network n) {
    return portsFor(n).rpc;
}

inline int wsPortFor(Network n) {
    return portsFor(n).ws;
}

// Legacy compatibility - keep old NetDefaults structure for gradual migration
struct NetDefaults {
    std::string name;
    int rpcPort;
    int wsPort;
    
    // Static constants for backward compatibility
    static constexpr int RPC = 20998;  // Default mainnet RPC port
    static constexpr int WSP = 21001;  // Default mainnet WS port
};

inline NetDefaults defaultsFor(Network n) {
    auto ports = portsFor(n);
    return {networkName(n), ports.rpc, ports.ws};
}

// Legacy din::defaults namespace for backward compatibility
namespace din::defaults {
    inline constexpr const char* kHost = "127.0.0.1";
    inline constexpr int kRpcPort = 20998;      // Mainnet default
    inline constexpr int kWsPort = 21001;       // Mainnet default
    inline constexpr int kReadyMs = 90000;      // 90 second timeout
}

// Conversion between GUI and daemon network enums
din::Network toDaemonNetwork(Network guiNetwork);
Network fromDaemonNetwork(din::Network daemonNetwork);