#include "p2p_message.h"

#ifdef QT_CORE_LIB
#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#else
#include "crypto/dinero_crypto_minimal.h"
#include <cstring>
#endif
#include "common/sha256d.h"
#ifdef QT_CORE_LIB
#include <QIODevice>
#endif

namespace p2p {

// Runtime network magic (defaults to mainnet)
uint32_t g_magic = NetworkMagic::MAINNET;

void init_p2p_network(const std::string& network) {
    if (network == "regtest") {
        g_magic = NetworkMagic::REGTEST;
    } else if (network == "testnet") {
        g_magic = NetworkMagic::TESTNET;
    } else {
        g_magic = NetworkMagic::MAINNET;
    }
#ifdef QT_CORE_LIB
    qDebug() << "[P2P] Network magic initialized:" << Qt::hex << g_magic << "for" << QString::fromStdString(network);
#endif
}

#ifdef QT_CORE_LIB
QByteArray dsha256(const QByteArray& b) {
    // Double SHA256 hash using dinero crypto
    Dinero::Common::sha256 hasher;
    hasher.update((const uint8_t*)b.constData(), b.size());
    std::vector<uint8_t> hash1 = hasher.finalize();
    
    hasher.reset();
    hasher.update(hash1.data(), hash1.size());
    std::vector<uint8_t> hash2 = hasher.finalize();
    
    return QByteArray((const char*)hash2.data(), 32);
}
#else
std::vector<uint8_t> dsha256(const std::vector<uint8_t>& b) {
    // Double SHA256 hash using dinero crypto
    Dinero::Common::sha256 hasher;
    hasher.update(b.data(), b.size());
    std::vector<uint8_t> hash1 = hasher.finalize();
    
    hasher.reset();
    hasher.update(hash1.data(), hash1.size());
    std::vector<uint8_t> hash2 = hasher.finalize();
    
    return hash2;
}
#endif

#ifdef QT_CORE_LIB
QByteArray makeMessage(const QByteArray& cmdIn, const QByteArray& payload) {
    // Prepare command (12 bytes, null-padded)
    QByteArray cmd = cmdIn.left(12);
    cmd.resize(12, '\0');
    
    // Calculate checksum (first 4 bytes of double SHA256)
    QByteArray hash = dsha256(payload);
    quint32 checksum;
    memcpy(&checksum, hash.constData(), 4);
#else
std::vector<uint8_t> makeMessage(const std::string& cmdIn, const std::vector<uint8_t>& payload) {
    // Prepare command (12 bytes, null-padded)
    std::string cmd = cmdIn.substr(0, 12);
    cmd.resize(12, '\0');
    
    // Calculate checksum (first 4 bytes of double SHA256)
    std::vector<uint8_t> hash = dsha256(payload);
    uint32_t checksum;
    memcpy(&checksum, hash.data(), 4);
#endif
    
    // Build message
    QByteArray out;
    out.reserve(sizeof(Header) + payload.size());
    
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    
    ds << g_magic;
    ds.writeRawData(cmd.constData(), 12);
    ds << quint32(payload.size());
    ds << checksum;
    
    out.push_back(payload);
    return out;
}

bool parseOne(QByteArray& in, QByteArray& outCmd, QByteArray& outPayload) {
    if (in.size() < int(sizeof(Header))) {
        return false;
    }
    
    QDataStream ds(in);
    ds.setByteOrder(QDataStream::LittleEndian);
    
    quint32 magic, length, checksum;
    char cmd[12];
    
    ds >> magic;
    if (magic != g_magic) {
        // Bad magic - consume one byte and try again
        in.remove(0, 1);
        return false;
    }
    
    ds.readRawData(cmd, 12);
    ds >> length >> checksum;
    
    int headerSize = sizeof(Header);
    if (in.size() < headerSize + int(length)) {
        return false; // Need more data
    }
    
    // Extract command (remove null padding)
    outCmd = QByteArray(cmd, 12);
    int nullPos = outCmd.indexOf('\0');
    if (nullPos >= 0) {
        outCmd = outCmd.left(nullPos);
    }
    
    // Extract payload
    outPayload = in.mid(headerSize, length);
    
    // Verify checksum
    QByteArray hash = dsha256(outPayload);
    quint32 calculatedChecksum;
    memcpy(&calculatedChecksum, hash.constData(), 4);
    
    if (calculatedChecksum != checksum) {
        // Bad checksum - remove this message and continue
        in.remove(0, headerSize + length);
        return false;
    }
    
    // Success - remove processed message from buffer
    in.remove(0, headerSize + length);
    return true;
}

} // namespace p2p
