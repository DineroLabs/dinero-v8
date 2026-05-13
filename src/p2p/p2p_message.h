#pragma once

#ifdef QT_CORE_LIB
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#else
#include <vector>
#include <string>
#include <cstdint>
#endif

namespace p2p {

// Network magic bytes - must match iOS Protocol.swift and p2p_wire_protocol.cpp
// These identify the network and prevent cross-network connections
namespace NetworkMagic {
    constexpr uint32_t MAINNET = 0xD1A0C0DEu;
    constexpr uint32_t TESTNET = 0xDAB5BFFAu;
    constexpr uint32_t REGTEST = 0xFABFB5DAu;
}

// Runtime-configurable network magic (set by init_p2p_network())
extern uint32_t g_magic;

// Initialize network magic based on network name ("mainnet", "testnet", "regtest")
void init_p2p_network(const std::string& network);

// Legacy constant for backwards compatibility (defaults to mainnet, use g_magic instead)
constexpr uint32_t MAGIC = NetworkMagic::MAINNET;

struct Header {
    uint32_t magic;
    char     command[12];
    uint32_t length;
    uint32_t checksum;
};

#ifdef QT_CORE_LIB
// Qt-based message functions
QByteArray makeMessage(const QByteArray& cmd, const QByteArray& payload);
bool parseOne(QByteArray& in, QByteArray& outCmd, QByteArray& outPayload);
QByteArray dsha256(const QByteArray& b);
#else
// Non-Qt message functions
std::vector<uint8_t> makeMessage(const std::string& cmd, const std::vector<uint8_t>& payload);
bool parseOne(std::vector<uint8_t>& in, std::string& outCmd, std::vector<uint8_t>& outPayload);
std::vector<uint8_t> dsha256(const std::vector<uint8_t>& b);
#endif

} // namespace p2p
