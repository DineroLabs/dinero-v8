// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/quic_session.h"

#include "network/quic_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
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

// Returns "now" in nanoseconds, in the same clock domain ngtcp2 uses for
// initial_ts/timestamps. Steady-clock since-epoch ns; ngtcp2 only cares
// that callers feed it a monotonic ns counter and stay consistent with it.
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
    // Role lives on Impl now (was public on QuicSession in the pre-task-2
    // header). Nested enum so all 5 of the existing Role::Client /
    // Role::Server references inside Impl methods keep compiling.
    enum class Role { Client, Server };
    enum class StartRequest { None, Client, Server };

    // --- Outbound wire-bytes callback, supplied at construction ---
    OutboundWriter outbound_writer;

    // --- Session-owned thread + queues ---
    std::thread session_thread;
    std::atomic<bool> stopping{false};

    std::mutex inbox_mutex;
    std::condition_variable inbox_cv;
    std::deque<std::vector<uint8_t>> incoming_packets;
    std::deque<std::pair<std::vector<uint8_t>, bool /*fin*/>> outgoing_streams;
    StartRequest pending_start{StartRequest::None};
    UdpAddr pending_local;
    UdpAddr pending_remote;
    QuicSessionOptions pending_options;
    // Server role records the supplied options once StartServer arrives;
    // the ngtcp2 server-side conn is created lazily on the first inbound
    // packet (see SessionLoop / ProcessIncomingPacket).
    QuicSessionOptions server_options;
    UdpAddr server_local;
    UdpAddr server_remote;
    bool server_primed{false};

    // Path addresses recorded at conn-creation time; used by
    // ProcessIncomingPacket when feeding bytes to ngtcp2_conn_read_pkt
    // (the listen thread does not pass per-packet addresses through
    // EnqueueIncomingPacket — single-relay, no path migration).
    UdpAddr session_local;
    UdpAddr session_remote;

    // Decrypted-stream outbox; consumed by handler thread via
    // QuicSession::ReadDecryptedStream. Separate lock to decouple from
    // inbox waiters.
    std::mutex outbox_mutex;
    std::condition_variable outbox_cv;
    std::deque<std::vector<uint8_t>> decrypted_outbox;

    // Handshake completion: session thread fulfils once, any thread waits.
    std::promise<bool> handshake_promise;
    std::shared_future<bool> handshake_future;
    bool handshake_promise_set{false};  // session-thread-only

    // Atomic snapshot fields read by const accessors from any thread.
    std::atomic<bool> active_published{false};
    std::atomic<bool> handshake_ready_published{false};

    // Last-error string with its own lock (rarely contended).
    mutable std::mutex error_mutex;
    std::string last_error_str;

    // --- ngtcp2 / TLS / connection state. Session-thread-only access. ---
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
    // Atomic — written from ngtcp2 callbacks on the session thread,
    // read by Stats() from any thread.
    std::atomic<bool> handshake_confirmed{false};
    std::atomic<bool> stream_closed{false};

    int64_t stream_id{-1};
    std::vector<uint8_t> send_buffer;
    size_t send_offset{0};
    bool send_fin{true};
    bool stream_fin_sent{false};

    std::vector<uint8_t> received_stream_data;
    // Snapshotted TLS info (cipher / ALPN) captured by the session thread
    // when handshake completes. Read by Stats() from any thread under
    // error_mutex (cheap; rarely contended).
    std::string tls_cipher_snapshot;
    std::string selected_alpn_snapshot;

    Impl() {
        conn_ref.get_conn = &Impl::GetConn;
        conn_ref.user_data = this;
        handshake_future = handshake_promise.get_future().share();
    }

    ~Impl() {
        Stop();
    }

    // Tear down ngtcp2 / OpenSSL state. Called on shutdown and at the top of
    // StartClient / StartServerFromInitial so we can reuse the same Impl for
    // a fresh handshake (preserves prior-API semantics).
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
        active_published.store(false);
        handshake_completed = false;
        handshake_confirmed.store(false);
        stream_closed.store(false);
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
            self->stream_closed.store(true);
        }
        return 0;
    }

    static int HandshakeCompleted(ngtcp2_conn*, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        self->handshake_completed = true;
        if (self->role == Role::Server) {
            self->handshake_confirmed.store(true);
        }
        return 0;
    }

    static int HandshakeConfirmed(ngtcp2_conn*, void* user_data) {
        static_cast<Impl*>(user_data)->handshake_confirmed.store(true);
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

    // Enable ngtcp2's keep-alive: when the connection is idle this long,
    // ngtcp2 emits a QUIC-level PING frame to keep the connection alive.
    // 20 seconds gives 3x safety margin under the 60s max_idle_timeout below.
    // Must be called AFTER ngtcp2_conn_*_new — there is no settings-level
    // equivalent; the API is per-conn (ngtcp2_conn_set_keep_alive_timeout).
    void EnableKeepAlive() {
        if (conn) {
            constexpr ngtcp2_duration kKeepAliveNs =
                20ULL * 1000ULL * 1000ULL * 1000ULL;
            ngtcp2_conn_set_keep_alive_timeout(conn, kKeepAliveNs);
        }
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
        // Idle timeout: 60 seconds. Raised from 30s because the keepalive_loop's
        // dineroid PING cadence is 30s — at the bare timeout we were racing
        // ourselves. With ngtcp2's keep_alive_period set to 20s (in settings
        // above), QUIC-level PING frames keep the connection alive between
        // application-layer messages; 60s timeout gives 3x safety margin.
        params.max_idle_timeout = 60000;
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

    // Session-thread-only: bring up the client-side ngtcp2 conn.
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
        EnableKeepAlive();

        if (!SetupClientTls()) {
            return false;
        }
        session_local = local;
        session_remote = remote;
        active = true;
        active_published.store(true);
        ClearError();
        return true;
    }

    // Session-thread-only: bring up the server-side ngtcp2 conn from the
    // first inbound packet (the QUIC Initial).
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
        EnableKeepAlive();

        if (!SetupServerTls()) {
            return false;
        }
        session_local = local;
        session_remote = remote;
        active = true;
        active_published.store(true);
        ClearError();
        return true;
    }

    // Session-thread-only: hand a wire packet to ngtcp2. Caller is expected
    // to have already brought up the conn (client via StartClient; server
    // via StartServerFromInitial on the first packet).
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

    // Session-thread-only: queue a single app-layer payload for the next
    // writev_stream call. Multi-payload queueing is handled at the
    // SessionLoop layer via the outgoing_streams deque; this method only
    // ever sees one queued chunk at a time.
    bool QueueStreamData(const std::vector<uint8_t>& payload, bool fin) {
        if (payload.empty()) {
            SetError("refusing to queue empty QUIC stream payload");
            return false;
        }
        if (!send_buffer.empty()) {
            SetError("a QUIC stream payload is already queued");
            return false;
        }
        if (stream_fin_sent) {
            SetError("the QUIC stream has already been closed");
            return false;
        }
        send_buffer = payload;
        send_offset = 0;
        send_fin = fin;
        return true;
    }

    bool MaybeOpenStream() {
        if (stream_id != -1 || send_buffer.empty() || !ConnHandshakeReady()) {
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
                    if (send_fin) {
                        stream_fin_sent = true;
                    } else {
                        send_buffer.clear();
                        send_offset = 0;
                    }
                }
            } else if (ndatalen == 0 && send_offset >= send_buffer.size()) {
                if (send_fin) {
                    stream_fin_sent = true;
                } else {
                    send_buffer.clear();
                    send_offset = 0;
                }
            }
        }
        return true;
    }

    std::vector<uint8_t> TakeReceivedStreamData() {
        auto out = std::move(received_stream_data);
        received_stream_data.clear();
        return out;
    }

    // Session-thread-only helper. Public Stats() reads a copy under
    // error_mutex; tls_cipher / selected_alpn snapshots are refreshed by
    // the session thread when the handshake completes.
    void CaptureTlsSnapshots() {
        if (!ssl) return;
        std::string cipher_str;
        std::string alpn_str;
        const char* cipher = SSL_get_cipher_name(ssl);
        if (cipher) cipher_str = cipher;
        const unsigned char* alpn = nullptr;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
        if (alpn && alpn_len > 0) {
            alpn_str.assign(reinterpret_cast<const char*>(alpn),
                            reinterpret_cast<const char*>(alpn) + alpn_len);
        }
        std::lock_guard<std::mutex> lock(error_mutex);
        tls_cipher_snapshot = std::move(cipher_str);
        selected_alpn_snapshot = std::move(alpn_str);
    }

    bool ConnHandshakeReady() const {
        return conn && ngtcp2_conn_get_handshake_completed(conn) != 0;
    }

    void SetError(std::string message) {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error_str = std::move(message);
    }

    void ClearError() {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error_str.clear();
    }

    // ------------------------------------------------------------------
    // Single-thread session loop. All ngtcp2_conn_* calls happen here.
    // ------------------------------------------------------------------

    static std::chrono::steady_clock::time_point NgtcpExpiryToSteady(
            Impl& impl, uint64_t expiry_ngtcp_ns) {
        const auto now_steady = std::chrono::steady_clock::now();
        const uint64_t now_ngtcp = Now();
        if (expiry_ngtcp_ns <= now_ngtcp) return now_steady;
        return now_steady + std::chrono::nanoseconds(expiry_ngtcp_ns - now_ngtcp);
    }

    static std::chrono::steady_clock::time_point ComputeNextWakeup(Impl& impl) {
        if (!impl.active_published.load() || !impl.conn) {
            return std::chrono::steady_clock::time_point::max();
        }
        const auto expiry_ns = ngtcp2_conn_get_expiry(impl.conn);
        if (expiry_ns == UINT64_MAX) {
            return std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        }
        return NgtcpExpiryToSteady(impl, expiry_ns);
    }

    // Returns false if ngtcp2 encountered a fatal error processing this
    // packet. The caller (SessionLoop) uses the return value to trigger
    // MaybePublishHandshakeFailed and Stop.
    static bool ProcessIncomingPacket(Impl& impl,
                                      const std::vector<uint8_t>& packet) {
        // Server-side lazy init: the first inbound packet brings up the
        // ngtcp2 server conn. After that, normal ReceivePacket.
        if (!impl.active_published.load()) {
            if (impl.server_primed) {
                if (!impl.StartServerFromInitial(impl.server_local,
                                                  impl.server_remote,
                                                  packet,
                                                  impl.server_options)) {
                    return false;
                }
                // Fall through: the Initial packet itself still needs to
                // be fed into ngtcp2_conn_read_pkt below, because
                // StartServerFromInitial only parsed enough to allocate
                // the conn. ngtcp2 will reparse the same bytes.
            } else {
                // Packet arrived before any Start*() — drop silently.
                return true;
            }
        }
        // Use the session's recorded path addresses; the relay listen
        // thread is single-path so there's no per-packet variation.
        return impl.ReceivePacket(impl.session_local, impl.session_remote, packet);
    }

    static void ProcessOutgoingStream(Impl& impl,
                                      std::vector<uint8_t> payload,
                                      bool fin) {
        if (!impl.active_published.load() || !impl.conn) {
            // No conn yet — drop. Callers should not enqueue before
            // StartClient/StartServer returns.
            return;
        }
        // QueueStreamData is single-slot; if a previous payload is still
        // in flight, requeue at the BACK so failed entries preserve their
        // relative order with other entries in this batch (front-push
        // would reverse a burst of N>1 enqueues — stream ordering bug).
        if (!impl.QueueStreamData(payload, fin)) {
            std::lock_guard<std::mutex> lock(impl.inbox_mutex);
            impl.outgoing_streams.emplace_back(std::move(payload), fin);
        }
    }

    static void DrainAndShip(Impl& impl) {
        if (!impl.active_published.load() || !impl.conn) return;
        std::vector<std::vector<uint8_t>> packets;
        if (!impl.DrainOutgoing(&packets)) return;
        for (auto& pkt : packets) {
            if (impl.outbound_writer) {
                impl.outbound_writer(std::move(pkt));
            }
        }
    }

    static void MaybePublishHandshakeReady(Impl& impl) {
        if (impl.handshake_promise_set) return;
        if (!impl.conn) return;
        if (ngtcp2_conn_get_handshake_completed(impl.conn)) {
            impl.CaptureTlsSnapshots();
            impl.handshake_ready_published.store(true);
            try {
                impl.handshake_promise.set_value(true);
            } catch (...) {
                // Already-set promise: should not happen given the
                // handshake_promise_set guard, but be defensive.
            }
            impl.handshake_promise_set = true;
        }
    }

    // Mirror of MaybePublishHandshakeReady for failure paths. Called from
    // SessionLoop whenever ngtcp2 signals a fatal error (ReceivePacket,
    // HandleExpiry, or StartClient failure). Idempotent via the same
    // handshake_promise_set guard.
    static void MaybePublishHandshakeFailed(Impl& impl) {
        if (impl.handshake_promise_set) return;
        impl.handshake_ready_published.store(false);
        try {
            impl.handshake_promise.set_value(false);
        } catch (...) {}
        impl.handshake_promise_set = true;
    }

    static void PublishDecryptedToOutbox(Impl& impl) {
        if (impl.received_stream_data.empty()) return;
        auto bytes = impl.TakeReceivedStreamData();
        if (bytes.empty()) return;
        {
            std::lock_guard<std::mutex> lock(impl.outbox_mutex);
            impl.decrypted_outbox.emplace_back(std::move(bytes));
        }
        impl.outbox_cv.notify_all();
    }

    static void SessionLoop(Impl& impl) {
        while (true) {
            std::unique_lock<std::mutex> lock(impl.inbox_mutex);
            const auto wakeup = ComputeNextWakeup(impl);
            impl.inbox_cv.wait_until(lock, wakeup, [&]() {
                return impl.stopping.load() ||
                       impl.pending_start != StartRequest::None ||
                       !impl.incoming_packets.empty() ||
                       !impl.outgoing_streams.empty();
            });

            const auto start_req = impl.pending_start;
            impl.pending_start = StartRequest::None;
            const UdpAddr start_local = impl.pending_local;
            const UdpAddr start_remote = impl.pending_remote;
            const QuicSessionOptions start_options = impl.pending_options;

            std::deque<std::vector<uint8_t>> incoming;
            std::swap(incoming, impl.incoming_packets);
            std::deque<std::pair<std::vector<uint8_t>, bool>> outgoing;
            std::swap(outgoing, impl.outgoing_streams);
            const bool stopping = impl.stopping.load();
            lock.unlock();

            // Track whether any ngtcp2 operation failed this iteration.
            bool ngtcp2_failed = false;

            if (start_req == StartRequest::Client) {
                if (!impl.StartClient(start_local, start_remote, start_options)) {
                    ngtcp2_failed = true;
                }
            } else if (start_req == StartRequest::Server) {
                impl.server_options = start_options;
                impl.server_local = start_local;
                impl.server_remote = start_remote;
                impl.server_primed = true;
                // Defer ngtcp2 init until the first inbound packet.
            }

            if (!ngtcp2_failed) {
                for (auto& packet : incoming) {
                    if (!ProcessIncomingPacket(impl, packet)) {
                        ngtcp2_failed = true;
                        break;
                    }
                }
            }
            if (!ngtcp2_failed) {
                for (auto& entry : outgoing) {
                    ProcessOutgoingStream(impl, std::move(entry.first), entry.second);
                }
            }
            if (!ngtcp2_failed && impl.active_published.load()) {
                if (!impl.HandleExpiry()) {
                    ngtcp2_failed = true;
                }
            }

            // On ngtcp2 failure: resolve the handshake future to false so
            // WaitHandshakeReady() callers get a prompt false ("it failed")
            // instead of timing out ("we don't know"). Then mark the session
            // dead so the loop exits cleanly.
            if (ngtcp2_failed) {
                MaybePublishHandshakeFailed(impl);
                impl.Stop();
                impl.stopping.store(true);
                impl.outbox_cv.notify_all();
            }
            DrainAndShip(impl);
            MaybePublishHandshakeReady(impl);
            PublishDecryptedToOutbox(impl);

            if (stopping) {
                // Drain whatever's left in one more pass, then exit.
                std::lock_guard<std::mutex> lock2(impl.inbox_mutex);
                if (impl.incoming_packets.empty() && impl.outgoing_streams.empty()) {
                    break;
                }
            }
        }
        // Wake any ReadDecryptedStream waiters so they unblock on close.
        impl.outbox_cv.notify_all();
    }
};

#else  // !DINERO_HAVE_NGTCP2 || !DINERO_HAVE_NGTCP2_CRYPTO_OSSL

// Stub Impl for builds without ngtcp2. Threading members exist so the
// constructor/destructor compile identically; the session thread is never
// started, and the public API methods early-return.
struct QuicSession::Impl {
    OutboundWriter outbound_writer;

    std::thread session_thread;
    std::atomic<bool> stopping{false};

    std::mutex inbox_mutex;
    std::condition_variable inbox_cv;
    std::deque<std::vector<uint8_t>> incoming_packets;
    std::deque<std::pair<std::vector<uint8_t>, bool>> outgoing_streams;

    std::mutex outbox_mutex;
    std::condition_variable outbox_cv;
    std::deque<std::vector<uint8_t>> decrypted_outbox;

    std::promise<bool> handshake_promise;
    std::shared_future<bool> handshake_future;
    bool handshake_promise_set{false};

    std::atomic<bool> active_published{false};
    std::atomic<bool> handshake_ready_published{false};

    mutable std::mutex error_mutex;
    std::string last_error_str{"ngtcp2 OpenSSL QUIC session support is not compiled in"};

    Impl() {
        handshake_future = handshake_promise.get_future().share();
    }
};

#endif  // DINERO_HAVE_NGTCP2 && DINERO_HAVE_NGTCP2_CRYPTO_OSSL

// =====================================================================
// Public API. All methods either enqueue work onto the session thread
// or read a published atomic snapshot. No public method touches ngtcp2.
// =====================================================================

QuicSession::QuicSession(OutboundWriter writer)
    : impl_(std::make_unique<Impl>()) {
    impl_->outbound_writer = std::move(writer);
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    impl_->session_thread = std::thread([impl = impl_.get()]() {
        Impl::SessionLoop(*impl);
    });
#endif
}

QuicSession::~QuicSession() {
    Close();
    if (impl_->session_thread.joinable()) {
        impl_->session_thread.join();
    }
    if (!impl_->handshake_promise_set) {
        try {
            impl_->handshake_promise.set_value(false);
            impl_->handshake_promise_set = true;
        } catch (...) {}
    }
}

bool QuicSession::StartClient(const UdpAddr& local,
                              const UdpAddr& remote,
                              const QuicSessionOptions& options) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->pending_start = Impl::StartRequest::Client;
        impl_->pending_local = local;
        impl_->pending_remote = remote;
        impl_->pending_options = options;
    }
    impl_->inbox_cv.notify_one();
    return true;
#else
    (void)local;
    (void)remote;
    (void)options;
    return false;
#endif
}

bool QuicSession::StartServer(const UdpAddr& local,
                              const UdpAddr& remote,
                              const QuicSessionOptions& options) {
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->pending_start = Impl::StartRequest::Server;
        impl_->pending_local = local;
        impl_->pending_remote = remote;
        impl_->pending_options = options;
    }
    impl_->inbox_cv.notify_one();
    return true;
#else
    (void)local;
    (void)remote;
    (void)options;
    return false;
#endif
}

void QuicSession::EnqueueIncomingPacket(std::vector<uint8_t> packet) {
    if (impl_->stopping.load()) return;  // drop post-Close; prevents destructor hang
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->incoming_packets.push_back(std::move(packet));
    }
    impl_->inbox_cv.notify_one();
}

void QuicSession::EnqueueOutgoingStream(std::vector<uint8_t> payload, bool fin) {
    if (impl_->stopping.load()) return;  // drop post-Close; prevents destructor hang
    {
        std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
        impl_->outgoing_streams.emplace_back(std::move(payload), fin);
    }
    impl_->inbox_cv.notify_one();
}

std::shared_future<bool> QuicSession::WaitHandshakeReady() const {
    return impl_->handshake_future;
}

std::vector<uint8_t> QuicSession::ReadDecryptedStream(
        std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->outbox_mutex);
    impl_->outbox_cv.wait_for(lock, timeout, [&]() {
        return !impl_->decrypted_outbox.empty() || impl_->stopping.load();
    });
    if (impl_->decrypted_outbox.empty()) return {};
    auto out = std::move(impl_->decrypted_outbox.front());
    impl_->decrypted_outbox.pop_front();
    return out;
}

bool QuicSession::active() const {
    return impl_->active_published.load();
}

bool QuicSession::handshake_ready() const {
    return impl_->handshake_ready_published.load();
}

std::string QuicSession::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->last_error_str;
}

void QuicSession::Close() {
    impl_->stopping.store(true);
    impl_->inbox_cv.notify_all();
    impl_->outbox_cv.notify_all();
}

QuicSessionStats QuicSession::Stats() const {
    QuicSessionStats stats;
    stats.active = impl_->active_published.load();
    stats.handshake_completed = impl_->handshake_ready_published.load();
#if defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL)
    stats.handshake_confirmed = impl_->handshake_confirmed.load();
    stats.stream_closed = impl_->stream_closed.load();
    // String snapshots are session-thread-published under error_mutex.
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    stats.tls_cipher = impl_->tls_cipher_snapshot;
    stats.selected_alpn = impl_->selected_alpn_snapshot;
#endif
    return stats;
}

}  // namespace dinero::network
