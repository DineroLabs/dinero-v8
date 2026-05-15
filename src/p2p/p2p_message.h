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

// Runtime-configurable network magic. Set by init_p2p_network() (below)
// from dinero::Params().magic — chainparams_impl.cpp is the single
// source of truth for the per-chain values. There are no hardcoded
// MAINNET/TESTNET/REGTEST literals in this file or anywhere else under
// src/ or include/; reach for dinero::Params().magic if you need one.
extern uint32_t g_magic;

// Initialize network magic based on network name ("mainnet", "testnet", "regtest").
// Drives dinero::SelectParams() and then reads dinero::Params().magic into g_magic.
void init_p2p_network(const std::string& network);

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
