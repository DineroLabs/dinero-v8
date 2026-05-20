// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/quic_transport.h"

#include <algorithm>
#include <mutex>
#include <utility>

#if defined(DINERO_HAVE_NGTCP2)
#include <ngtcp2/ngtcp2.h>
#endif

#if defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/crypto.h>
#elif defined(DINERO_HAVE_NGTCP2_CRYPTO_QUICTLS)
#include <ngtcp2/ngtcp2_crypto_quictls.h>
#include <openssl/crypto.h>
#endif

namespace dinero::network {
namespace {

std::once_flag g_crypto_init_once;
bool g_crypto_init_ok = false;

const char* CryptoBackendName() {
#if defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return "ossl";
#elif defined(DINERO_HAVE_NGTCP2_CRYPTO_QUICTLS)
    return "quictls";
#else
    return "none";
#endif
}

bool CryptoCompiledIn() {
#if defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL) || defined(DINERO_HAVE_NGTCP2_CRYPTO_QUICTLS)
    return true;
#else
    return false;
#endif
}

}  // namespace

QuicTransport::QuicTransport() = default;

QuicTransport::~QuicTransport() {
    Stop();
}

QuicTransportInfo QuicTransport::CompileInfo() {
    QuicTransportInfo info;
    info.crypto_backend = CryptoBackendName();

#if defined(DINERO_HAVE_NGTCP2)
    info.ngtcp2_available = true;
    if (const ngtcp2_info* ngtcp2_info = ngtcp2_version(0)) {
        info.ngtcp2_version = ngtcp2_info->version_str ? ngtcp2_info->version_str : "";
    }
#else
    info.disabled_reason = "ngtcp2 support was not compiled in";
    return info;
#endif

    info.crypto_available = CryptoCompiledIn();
#if defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL) || defined(DINERO_HAVE_NGTCP2_CRYPTO_QUICTLS)
    info.openssl_version = OpenSSL_version(OPENSSL_VERSION);
#endif

    info.mainnet_relay_ready = MainnetRelayReady();
    if (!info.crypto_available) {
        info.disabled_reason = "ngtcp2 is present but no compatible OpenSSL QUIC crypto bridge was compiled";
    } else if (!info.mainnet_relay_ready) {
        info.disabled_reason =
            "encrypted QUIC stream/session plumbing is not wired into P2PManager yet";
    }
    return info;
}

bool QuicTransport::InitializeCrypto() {
#if defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    std::call_once(g_crypto_init_once, [] {
        g_crypto_init_ok = (ngtcp2_crypto_ossl_init() == 0);
    });
    return g_crypto_init_ok;
#elif defined(DINERO_HAVE_NGTCP2_CRYPTO_QUICTLS)
    std::call_once(g_crypto_init_once, [] {
        g_crypto_init_ok = (ngtcp2_crypto_quictls_init() == 0);
    });
    return g_crypto_init_ok;
#else
    return false;
#endif
}

bool QuicTransport::MainnetRelayReady() {
    return false;
}

bool QuicTransport::Start(const Options& options) {
    if (udp_.active()) {
        SetLastError("QUIC transport is already active");
        return false;
    }

    if (!InitializeCrypto()) {
        SetLastError("QUIC crypto bridge is not available");
        return false;
    }

    max_pending_datagrams_ = std::max<size_t>(1, options.max_pending_datagrams);
    udp_.OnReceive([this](const UdpAddr& source, std::vector<uint8_t> payload) {
        OnDatagram(source, std::move(payload));
    });

    if (!udp_.Bind(options.listen_port)) {
        SetLastError("failed to bind UDP socket for QUIC transport");
        udp_.OnReceive(nullptr);
        return false;
    }

    SetLastError({});
    return true;
}

void QuicTransport::Stop() {
    udp_.Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_datagrams_.clear();
    }
    datagram_cv_.notify_all();
}

bool QuicTransport::SendDatagram(const UdpAddr& destination,
                                 const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        SetLastError("refusing to send empty QUIC datagram");
        return false;
    }
    if (!udp_.SendTo(destination, payload.data(), payload.size())) {
        SetLastError("failed to send QUIC UDP datagram");
        return false;
    }
    return true;
}

bool QuicTransport::ReceiveDatagram(QuicDatagram* out,
                                    std::chrono::milliseconds timeout) {
    if (!out) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = datagram_cv_.wait_for(lock, timeout, [this] {
        return !pending_datagrams_.empty() || !udp_.active();
    });
    if (!ready || pending_datagrams_.empty()) {
        return false;
    }

    *out = std::move(pending_datagrams_.front());
    pending_datagrams_.pop_front();
    return true;
}

bool QuicTransport::active() const {
    return udp_.active();
}

uint16_t QuicTransport::bound_port() const {
    return udp_.bound_port();
}

std::string QuicTransport::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void QuicTransport::OnDatagram(const UdpAddr& source, std::vector<uint8_t> payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_datagrams_.size() >= max_pending_datagrams_) {
            pending_datagrams_.pop_front();
        }
        pending_datagrams_.push_back(QuicDatagram{source, std::move(payload)});
    }
    datagram_cv_.notify_one();
}

void QuicTransport::SetLastError(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(message);
}

}  // namespace dinero::network
