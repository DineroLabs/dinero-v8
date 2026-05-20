// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/quic_session.h"

#include "network/quic_transport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#endif

namespace dinero::network {

#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
namespace {

constexpr size_t kCidLen = 16;
constexpr size_t kMaxPacketsPerDrain = 64;
constexpr uint8_t kStatelessResetSecret[] = "dinero-quic-reset-secret-v1";

ngtcp2_tstamp Now() {
    using namespace std::chrono;
    return static_cast<ngtcp2_tstamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

bool RandomBytes(uint8_t* dest, size_t len) {
    return RAND_bytes(dest, static_cast<int>(len)) == 1;
}

std::string OpenSslError() {
    const auto err = ERR_get_error();
    if (err == 0) {
        return {};
    }
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return buf;
}

std::vector<unsigned char> LengthPrefixedAlpn(const std::string& alpn) {
    std::vector<unsigned char> out;
    if (alpn.empty() || alpn.size() > 255) {
        return out;
    }
    out.push_back(static_cast<unsigned char>(alpn.size()));
    out.insert(out.end(), alpn.begin(), alpn.end());
    return out;
}

bool MakeNgAddr(const UdpAddr& addr,
                sockaddr_storage* storage,
                ngtcp2_addr* out) {
    std::memset(storage, 0, sizeof(*storage));
    if (addr.family == UdpAddr::Family::V4) {
        auto* in = reinterpret_cast<sockaddr_in*>(storage);
        in->sin_family = AF_INET;
        in->sin_port = htons(addr.port);
        std::memcpy(&in->sin_addr.s_addr, addr.ip.data(), 4);
        out->addr = reinterpret_cast<sockaddr*>(storage);
        out->addrlen = static_cast<ngtcp2_socklen>(sizeof(sockaddr_in));
        return true;
    }
    if (addr.family == UdpAddr::Family::V6) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(storage);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(addr.port);
        std::memcpy(&in6->sin6_addr, addr.ip.data(), 16);
        out->addr = reinterpret_cast<sockaddr*>(storage);
        out->addrlen = static_cast<ngtcp2_socklen>(sizeof(sockaddr_in6));
        return true;
    }
    return false;
}

}  // namespace

struct QuicSession::Impl {
    Role role{Role::Client};
    QuicSessionOptions options;

    ngtcp2_conn* conn{nullptr};
    ngtcp2_crypto_conn_ref conn_ref{};
    ngtcp2_crypto_ossl_ctx* crypto_ctx{nullptr};
    SSL_CTX* ssl_ctx{nullptr};
    SSL* ssl{nullptr};

    sockaddr_storage local_storage{};
    sockaddr_storage remote_storage{};
    ngtcp2_path path{};

    bool active{false};
    bool handshake_completed{false};
    bool handshake_confirmed{false};
    bool stream_closed{false};

    int64_t stream_id{-1};
    std::vector<uint8_t> send_buffer;
    size_t send_offset{0};
    bool send_fin{true};
    bool stream_fin_sent{false};

    std::vector<uint8_t> received_stream_data;
    std::string last_error;

    Impl() {
        conn_ref.get_conn = &Impl::GetConn;
        conn_ref.user_data = this;
    }

    ~Impl() {
        Stop();
    }

    void Stop() {
        if (ssl) {
            SSL_set_app_data(ssl, nullptr);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ssl_ctx) {
            SSL_CTX_free(ssl_ctx);
            ssl_ctx = nullptr;
        }
        if (crypto_ctx) {
            ngtcp2_crypto_ossl_ctx_del(crypto_ctx);
            crypto_ctx = nullptr;
        }
        if (conn) {
            ngtcp2_conn_del(conn);
            conn = nullptr;
        }
        active = false;
        handshake_completed = false;
        handshake_confirmed = false;
        stream_closed = false;
        stream_id = -1;
        send_buffer.clear();
        send_offset = 0;
        stream_fin_sent = false;
        received_stream_data.clear();
    }

    static ngtcp2_conn* GetConn(ngtcp2_crypto_conn_ref* ref) {
        return static_cast<Impl*>(ref->user_data)->conn;
    }

    static void Rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*) {
        (void)RandomBytes(dest, destlen);
    }

    static int GetNewConnectionId(ngtcp2_conn*,
                                  ngtcp2_cid* cid,
                                  ngtcp2_stateless_reset_token* token,
                                  size_t cidlen,
                                  void*) {
        if (!RandomBytes(cid->data, cidlen)) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        cid->datalen = cidlen;
        if (ngtcp2_crypto_generate_stateless_reset_token(
                token->data,
                kStatelessResetSecret,
                sizeof(kStatelessResetSecret) - 1,
                cid) != 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }

    static int RecvStreamData(ngtcp2_conn* conn,
                              uint32_t flags,
                              int64_t stream_id,
                              uint64_t,
                              const uint8_t* data,
                              size_t datalen,
                              void* user_data,
                              void*) {
        auto* self = static_cast<Impl*>(user_data);
        self->received_stream_data.insert(self->received_stream_data.end(),
                                          data,
                                          data + datalen);
        ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
        ngtcp2_conn_extend_max_offset(conn, datalen);
        if ((flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0) {
            self->stream_closed = true;
        }
        return 0;
    }

    static int HandshakeCompleted(ngtcp2_conn*, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        self->handshake_completed = true;
        if (self->role == Role::Server) {
            self->handshake_confirmed = true;
        }
        return 0;
    }

    static int HandshakeConfirmed(ngtcp2_conn*, void* user_data) {
        static_cast<Impl*>(user_data)->handshake_confirmed = true;
        return 0;
    }

    static int SelectAlpn(SSL*,
                          const unsigned char** out,
                          unsigned char* outlen,
                          const unsigned char* in,
                          unsigned int inlen,
                          void* arg) {
        auto* self = static_cast<Impl*>(arg);
        const auto& alpn = self->options.alpn;
        for (unsigned int i = 0; i < inlen;) {
            const unsigned int len = in[i++];
            if (i + len > inlen) {
                break;
            }
            if (len == alpn.size() &&
                std::equal(alpn.begin(), alpn.end(), in + i)) {
                *out = in + i;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            i += len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    ngtcp2_callbacks BuildCallbacks() {
        ngtcp2_callbacks callbacks{};
        if (role == Role::Client) {
            callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
            callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
        } else {
            callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
        }
        callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        callbacks.handshake_completed = &Impl::HandshakeCompleted;
        callbacks.handshake_confirmed = &Impl::HandshakeConfirmed;
        callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
        callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
        callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
        callbacks.recv_stream_data = &Impl::RecvStreamData;
        callbacks.rand = &Impl::Rand;
        callbacks.update_key = ngtcp2_crypto_update_key_cb;
        callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        callbacks.get_new_connection_id2 = &Impl::GetNewConnectionId;
        callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        return callbacks;
    }

    ngtcp2_settings BuildSettings() const {
        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = Now();
        settings.max_tx_udp_payload_size =
            std::max<size_t>(NGTCP2_MAX_UDP_PAYLOAD_SIZE, options.max_datagram_size);
        return settings;
    }

    ngtcp2_transport_params BuildTransportParams() const {
        ngtcp2_transport_params params;
        ngtcp2_transport_params_default(&params);
        params.initial_max_stream_data_bidi_local = 1024 * 1024;
        params.initial_max_stream_data_bidi_remote = 1024 * 1024;
        params.initial_max_stream_data_uni = 1024 * 1024;
        params.initial_max_data = 4 * 1024 * 1024;
        params.initial_max_streams_bidi = 16;
        params.initial_max_streams_uni = 4;
        params.max_idle_timeout = 30000;
        params.active_connection_id_limit = 4;
        return params;
    }

    bool PreparePath(const UdpAddr& local, const UdpAddr& remote) {
        if (!MakeNgAddr(local, &local_storage, &path.local) ||
            !MakeNgAddr(remote, &remote_storage, &path.remote)) {
            SetError("invalid QUIC session UDP address");
            return false;
        }
        path.user_data = this;
        return true;
    }

    bool CreateCryptoContext() {
        if (ngtcp2_crypto_ossl_ctx_new(&crypto_ctx, nullptr) != 0) {
            SetError("failed to allocate ngtcp2 OpenSSL crypto context");
            return false;
        }
        return true;
    }

    bool SetupClientTls() {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            SetError("SSL_CTX_new(client) failed: " + OpenSslError());
            return false;
        }
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
        SSL_CTX_set_verify(ssl_ctx,
                           options.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                           nullptr);
        if (options.verify_peer) {
            SSL_CTX_set_default_verify_paths(ssl_ctx);
        }

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            SetError("SSL_new(client) failed: " + OpenSslError());
            return false;
        }
        ngtcp2_crypto_ossl_ctx_set_ssl(crypto_ctx, ssl);
        if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0) {
            SetError("failed to configure OpenSSL QUIC client session");
            return false;
        }
        SSL_set_app_data(ssl, &conn_ref);
        SSL_set_connect_state(ssl);
        SSL_set_tlsext_host_name(ssl, options.server_name.c_str());

        const auto alpn = LengthPrefixedAlpn(options.alpn);
        if (alpn.empty() ||
            SSL_set_alpn_protos(ssl, alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            SetError("failed to configure client ALPN");
            return false;
        }

        ngtcp2_conn_set_tls_native_handle(conn, crypto_ctx);
        return true;
    }

    bool LoadServerCertificate() {
        BIO* cert_bio = BIO_new_mem_buf(options.certificate_pem.data(),
                                        static_cast<int>(options.certificate_pem.size()));
        if (!cert_bio) {
            SetError("failed to create certificate BIO");
            return false;
        }
        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        BIO_free(cert_bio);
        if (!cert) {
            SetError("failed to read server certificate PEM: " + OpenSslError());
            return false;
        }
        const int cert_ok = SSL_CTX_use_certificate(ssl_ctx, cert);
        X509_free(cert);
        if (cert_ok != 1) {
            SetError("failed to load server certificate: " + OpenSslError());
            return false;
        }

        BIO* key_bio = BIO_new_mem_buf(options.private_key_pem.data(),
                                       static_cast<int>(options.private_key_pem.size()));
        if (!key_bio) {
            SetError("failed to create private-key BIO");
            return false;
        }
        EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        if (!key) {
            SetError("failed to read server private-key PEM: " + OpenSslError());
            return false;
        }
        const int key_ok = SSL_CTX_use_PrivateKey(ssl_ctx, key);
        EVP_PKEY_free(key);
        if (key_ok != 1 || SSL_CTX_check_private_key(ssl_ctx) != 1) {
            SetError("failed to load server private key: " + OpenSslError());
            return false;
        }
        return true;
    }

    bool SetupServerTls() {
        if (options.certificate_pem.empty() || options.private_key_pem.empty()) {
            SetError("server QUIC sessions require certificate and private-key PEM");
            return false;
        }

        ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx) {
            SetError("SSL_CTX_new(server) failed: " + OpenSslError());
            return false;
        }
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
        SSL_CTX_set_alpn_select_cb(ssl_ctx, &Impl::SelectAlpn, this);
        SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);
        const unsigned char sid_ctx[] = "dinero-quic-session";
        SSL_CTX_set_session_id_context(ssl_ctx, sid_ctx, sizeof(sid_ctx) - 1);

        if (!LoadServerCertificate()) {
            return false;
        }

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            SetError("SSL_new(server) failed: " + OpenSslError());
            return false;
        }
        ngtcp2_crypto_ossl_ctx_set_ssl(crypto_ctx, ssl);
        if (ngtcp2_crypto_ossl_configure_server_session(ssl) != 0) {
            SetError("failed to configure OpenSSL QUIC server session");
            return false;
        }
        SSL_set_app_data(ssl, &conn_ref);
        SSL_set_accept_state(ssl);

        ngtcp2_conn_set_tls_native_handle(conn, crypto_ctx);
        return true;
    }

    bool StartClient(const UdpAddr& local,
                     const UdpAddr& remote,
                     const QuicSessionOptions& opts) {
        Stop();
        role = Role::Client;
        options = opts;
        options.max_datagram_size =
            std::max<size_t>(NGTCP2_MAX_UDP_PAYLOAD_SIZE, opts.max_datagram_size);
        if (!QuicTransport::InitializeCrypto()) {
            SetError("QUIC crypto bridge is not available");
            return false;
        }
        if (!PreparePath(local, remote) || !CreateCryptoContext()) {
            return false;
        }

        ngtcp2_cid dcid;
        ngtcp2_cid scid;
        dcid.datalen = kCidLen;
        scid.datalen = kCidLen;
        if (!RandomBytes(dcid.data, dcid.datalen) ||
            !RandomBytes(scid.data, scid.datalen)) {
            SetError("failed to generate QUIC connection IDs");
            return false;
        }

        auto callbacks = BuildCallbacks();
        auto settings = BuildSettings();
        auto params = BuildTransportParams();
        const int rv = ngtcp2_conn_client_new(&conn,
                                              &dcid,
                                              &scid,
                                              &path,
                                              NGTCP2_PROTO_VER_V1,
                                              &callbacks,
                                              &settings,
                                              &params,
                                              nullptr,
                                              this);
        if (rv != 0) {
            SetError(std::string("ngtcp2_conn_client_new failed: ") + ngtcp2_strerror(rv));
            return false;
        }

        if (!SetupClientTls()) {
            return false;
        }
        active = true;
        last_error.clear();
        return true;
    }

    bool StartServerFromInitial(const UdpAddr& local,
                                const UdpAddr& remote,
                                const std::vector<uint8_t>& first_packet,
                                const QuicSessionOptions& opts) {
        Stop();
        role = Role::Server;
        options = opts;
        options.max_datagram_size =
            std::max<size_t>(NGTCP2_MAX_UDP_PAYLOAD_SIZE, opts.max_datagram_size);
        if (!QuicTransport::InitializeCrypto()) {
            SetError("QUIC crypto bridge is not available");
            return false;
        }
        if (first_packet.size() < 21) {
            SetError("first QUIC packet is too short");
            return false;
        }
        if (!PreparePath(local, remote) || !CreateCryptoContext()) {
            return false;
        }

        ngtcp2_version_cid vcid;
        int rv = ngtcp2_pkt_decode_version_cid(&vcid,
                                               first_packet.data(),
                                               first_packet.size(),
                                               kCidLen);
        if (rv != 0) {
            SetError(std::string("failed to decode Initial packet CID: ") +
                     ngtcp2_strerror(rv));
            return false;
        }

        ngtcp2_pkt_hd hd;
        rv = ngtcp2_accept(&hd, first_packet.data(), first_packet.size());
        if (rv != 0) {
            SetError(std::string("first packet is not an acceptable QUIC Initial: ") +
                     ngtcp2_strerror(rv));
            return false;
        }

        ngtcp2_cid dcid;
        ngtcp2_cid_init(&dcid, vcid.scid, vcid.scidlen);

        ngtcp2_cid scid;
        scid.datalen = kCidLen;
        if (!RandomBytes(scid.data, scid.datalen)) {
            SetError("failed to generate server QUIC connection ID");
            return false;
        }

        auto callbacks = BuildCallbacks();
        auto settings = BuildSettings();
        auto params = BuildTransportParams();
        ngtcp2_cid_init(&params.original_dcid, vcid.dcid, vcid.dcidlen);
        params.original_dcid_present = 1;
        if (ngtcp2_crypto_generate_stateless_reset_token(
                params.stateless_reset_token,
                kStatelessResetSecret,
                sizeof(kStatelessResetSecret) - 1,
                &scid) != 0) {
            SetError("failed to generate server stateless reset token");
            return false;
        }

        rv = ngtcp2_conn_server_new(&conn,
                                    &dcid,
                                    &scid,
                                    &path,
                                    vcid.version,
                                    &callbacks,
                                    &settings,
                                    &params,
                                    nullptr,
                                    this);
        if (rv != 0) {
            SetError(std::string("ngtcp2_conn_server_new failed: ") + ngtcp2_strerror(rv));
            return false;
        }

        if (!SetupServerTls()) {
            return false;
        }
        active = true;
        last_error.clear();
        return true;
    }

    bool ReceivePacket(const UdpAddr& local,
                       const UdpAddr& remote,
                       const std::vector<uint8_t>& packet) {
        if (!active || !conn) {
            SetError("QUIC session is not active");
            return false;
        }
        sockaddr_storage local_ss;
        sockaddr_storage remote_ss;
        ngtcp2_path read_path;
        if (!MakeNgAddr(local, &local_ss, &read_path.local) ||
            !MakeNgAddr(remote, &remote_ss, &read_path.remote)) {
            SetError("invalid QUIC packet path");
            return false;
        }
        read_path.user_data = this;

        const int rv = ngtcp2_conn_read_pkt(conn,
                                            &read_path,
                                            nullptr,
                                            packet.data(),
                                            packet.size(),
                                            Now());
        if (rv != 0) {
            SetError(std::string("ngtcp2_conn_read_pkt failed: ") + ngtcp2_strerror(rv));
            return false;
        }
        return true;
    }

    bool HandleExpiry() {
        if (!active || !conn) {
            return false;
        }
        const int rv = ngtcp2_conn_handle_expiry(conn, Now());
        if (rv != 0) {
            SetError(std::string("ngtcp2_conn_handle_expiry failed: ") +
                     ngtcp2_strerror(rv));
            return false;
        }
        return true;
    }

    bool QueueStreamData(const std::vector<uint8_t>& payload, bool fin) {
        if (payload.empty()) {
            SetError("refusing to queue empty QUIC stream payload");
            return false;
        }
        if (!send_buffer.empty() && !stream_fin_sent) {
            SetError("a QUIC stream payload is already queued");
            return false;
        }
        send_buffer = payload;
        send_offset = 0;
        send_fin = fin;
        stream_fin_sent = false;
        return true;
    }

    bool MaybeOpenStream() {
        if (stream_id != -1 || send_buffer.empty() || !handshake_ready()) {
            return true;
        }
        if (ngtcp2_conn_get_streams_bidi_left(conn) == 0) {
            return true;
        }
        const int rv = ngtcp2_conn_open_bidi_stream(conn, &stream_id, nullptr);
        if (rv != 0) {
            SetError(std::string("ngtcp2_conn_open_bidi_stream failed: ") +
                     ngtcp2_strerror(rv));
            return false;
        }
        return true;
    }

    bool DrainOutgoing(std::vector<std::vector<uint8_t>>* packets) {
        if (!packets) {
            return false;
        }
        packets->clear();
        if (!active || !conn) {
            SetError("QUIC session is not active");
            return false;
        }
        if (!MaybeOpenStream()) {
            return false;
        }

        for (size_t i = 0; i < kMaxPacketsPerDrain; ++i) {
            std::vector<uint8_t> buf(options.max_datagram_size);
            ngtcp2_path_storage ps;
            ngtcp2_path_storage_zero(&ps);
            ngtcp2_ssize ndatalen = -1;

            int64_t write_stream_id = -1;
            ngtcp2_vec vec;
            const ngtcp2_vec* vecs = nullptr;
            size_t veccnt = 0;
            uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
            if (stream_id != -1 && !stream_fin_sent && send_offset < send_buffer.size()) {
                write_stream_id = stream_id;
                vec.base = send_buffer.data() + send_offset;
                vec.len = send_buffer.size() - send_offset;
                vecs = &vec;
                veccnt = 1;
                if (send_fin) {
                    flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
                }
            }

            const auto nwrite = ngtcp2_conn_writev_stream(conn,
                                                          &ps.path,
                                                          nullptr,
                                                          buf.data(),
                                                          buf.size(),
                                                          &ndatalen,
                                                          flags,
                                                          write_stream_id,
                                                          vecs,
                                                          veccnt,
                                                          Now());
            if (nwrite < 0) {
                if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                    nwrite == NGTCP2_ERR_STREAM_ID_BLOCKED) {
                    break;
                }
                SetError(std::string("ngtcp2_conn_writev_stream failed: ") +
                         ngtcp2_strerror(static_cast<int>(nwrite)));
                return false;
            }
            if (nwrite == 0) {
                break;
            }

            ngtcp2_conn_update_pkt_tx_time(conn, Now());
            buf.resize(static_cast<size_t>(nwrite));
            packets->push_back(std::move(buf));

            if (ndatalen > 0) {
                send_offset += static_cast<size_t>(ndatalen);
                if (send_offset >= send_buffer.size()) {
                    stream_fin_sent = send_fin;
                }
            } else if (ndatalen == 0 && send_offset >= send_buffer.size()) {
                stream_fin_sent = send_fin;
            }
        }
        return true;
    }

    std::vector<uint8_t> TakeReceivedStreamData() {
        auto out = std::move(received_stream_data);
        received_stream_data.clear();
        return out;
    }

    QuicSessionStats Stats() const {
        QuicSessionStats stats;
        stats.active = active;
        stats.handshake_completed =
            handshake_completed || (conn && ngtcp2_conn_get_handshake_completed(conn));
        stats.handshake_confirmed = handshake_confirmed;
        stats.stream_closed = stream_closed;
        if (ssl) {
            const char* cipher = SSL_get_cipher_name(ssl);
            if (cipher) {
                stats.tls_cipher = cipher;
            }
            const unsigned char* alpn = nullptr;
            unsigned int alpn_len = 0;
            SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
            if (alpn && alpn_len > 0) {
                stats.selected_alpn.assign(reinterpret_cast<const char*>(alpn),
                                           reinterpret_cast<const char*>(alpn) + alpn_len);
            }
        }
        return stats;
    }

    bool handshake_ready() const {
        return conn && ngtcp2_conn_get_handshake_completed(conn) != 0;
    }

    void SetError(std::string message) {
        last_error = std::move(message);
    }
};

#else

struct QuicSession::Impl {
    std::string last_error{"ngtcp2 OpenSSL QUIC session support is not compiled in"};
};

#endif

QuicSession::QuicSession() : impl_(new Impl()) {}

QuicSession::~QuicSession() {
    delete impl_;
}

bool QuicSession::StartClient(const UdpAddr& local,
                              const UdpAddr& remote,
                              const QuicSessionOptions& options) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->StartClient(local, remote, options);
#else
    (void)local;
    (void)remote;
    (void)options;
    return false;
#endif
}

bool QuicSession::StartServerFromInitial(const UdpAddr& local,
                                         const UdpAddr& remote,
                                         const std::vector<uint8_t>& first_packet,
                                         const QuicSessionOptions& options) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->StartServerFromInitial(local, remote, first_packet, options);
#else
    (void)local;
    (void)remote;
    (void)first_packet;
    (void)options;
    return false;
#endif
}

bool QuicSession::ReceivePacket(const UdpAddr& local,
                                const UdpAddr& remote,
                                const std::vector<uint8_t>& packet) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->ReceivePacket(local, remote, packet);
#else
    (void)local;
    (void)remote;
    (void)packet;
    return false;
#endif
}

bool QuicSession::HandleExpiry() {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->HandleExpiry();
#else
    return false;
#endif
}

bool QuicSession::QueueStreamData(const std::vector<uint8_t>& payload, bool fin) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->QueueStreamData(payload, fin);
#else
    (void)payload;
    (void)fin;
    return false;
#endif
}

bool QuicSession::DrainOutgoing(std::vector<std::vector<uint8_t>>* packets) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->DrainOutgoing(packets);
#else
    if (packets) {
        packets->clear();
    }
    return false;
#endif
}

std::vector<uint8_t> QuicSession::TakeReceivedStreamData() {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->TakeReceivedStreamData();
#else
    return {};
#endif
}

QuicSessionStats QuicSession::Stats() const {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->Stats();
#else
    return {};
#endif
}

bool QuicSession::active() const {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->active;
#else
    return false;
#endif
}

bool QuicSession::handshake_ready() const {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    return impl_->handshake_ready();
#else
    return false;
#endif
}

std::string QuicSession::last_error() const {
    return impl_->last_error;
}

}  // namespace dinero::network
