// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/tor_control.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace dinero::network {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) { if (socket != kInvalidSocket) closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket socket) { if (socket != kInvalidSocket) close(socket); }
#endif

std::string Trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 15]);
    }
    return out;
}

std::string QuoteControlString(const std::string& value) {
    std::string out{"\""};
    for (char c : value) {
        if (c == '\\' || c == '\"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('\"');
    return out;
}

bool ReadFileBinary(const std::string& path, std::vector<uint8_t>* bytes) {
    if (!bytes || path.empty()) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !bytes->empty();
}

}  // namespace

bool WriteTorPrivateKey(const std::string& path, const std::string& key,
                        std::string* error) {
    if (path.empty() || key.empty()) {
        if (error) *error = "onion private-key path or key is empty";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output || !(output << key << '\n')) {
            if (error) *error = "could not write onion private key";
            return false;
        }
        output.flush();
        if (!output) {
            if (error) *error = "could not flush onion private key";
            return false;
        }
    }
#ifndef _WIN32
    if (chmod(tmp.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not restrict onion private key permissions";
        return false;
    }
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        if (error) *error = "could not install onion private key: " + ec.message();
        return false;
    }
    return true;
}

std::string ReadTorPrivateKey(const std::string& path) {
#ifndef _WIN32
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return {};
    }
#endif
    std::ifstream input(path, std::ios::binary);
    std::string key;
    std::getline(input, key);
    key = Trim(key);
    return key.rfind("ED25519-V3:", 0) == 0 ? key : std::string{};
}

namespace {

class ControlConnection {
public:
    ~ControlConnection() { CloseSocket(socket_); }

    bool Connect(const TorOnionServiceConfig& config, std::string* error) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error) *error = "WSAStartup failed";
            return false;
        }
#endif
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(config.control_port);
        if (getaddrinfo(config.control_host.c_str(), port.c_str(), &hints, &addresses) != 0) {
            if (error) *error = "Tor control host resolution failed";
            return false;
        }
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            socket_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket_ == kInvalidSocket) continue;
#ifdef _WIN32
            DWORD timeout = static_cast<DWORD>(config.timeout_seconds * 1000);
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
            timeval timeout{config.timeout_seconds, 0};
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
            if (connect(socket_, address->ai_addr,
                        static_cast<socklen_t>(address->ai_addrlen)) == 0) break;
            CloseSocket(socket_);
            socket_ = kInvalidSocket;
        }
        freeaddrinfo(addresses);
        if (socket_ == kInvalidSocket) {
            if (error) *error = "could not connect to Tor control endpoint";
            return false;
        }
        return true;
    }

    bool Command(const std::string& command, TorControlReply* reply,
                 std::string* error) {
        const std::string wire = command + "\r\n";
        size_t sent = 0;
        while (sent < wire.size()) {
            const int result = send(socket_, wire.data() + sent,
                                    static_cast<int>(wire.size() - sent), 0);
            if (result <= 0) {
                if (error) *error = "Tor control send failed";
                return false;
            }
            sent += static_cast<size_t>(result);
        }

        std::string response;
        char buffer[1024];
        while (response.size() < 64 * 1024) {
            const int received = recv(socket_, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                if (error) *error = "Tor control response timed out or closed";
                return false;
            }
            response.append(buffer, static_cast<size_t>(received));
            size_t start = 0;
            while (start < response.size()) {
                const size_t end = response.find("\r\n", start);
                if (end == std::string::npos) break;
                if (end >= start + 4 && std::isdigit(response[start]) &&
                    response[start + 3] == ' ') {
                    return ParseTorControlReply(response.substr(0, end + 2), reply, error);
                }
                start = end + 2;
            }
        }
        if (error) *error = "Tor control response exceeded limit";
        return false;
    }

private:
    Socket socket_{kInvalidSocket};
};

std::string ExtractValue(const TorControlReply& reply, const std::string& key) {
    for (const auto& line : reply.lines) {
        const size_t pos = line.find(key);
        if (pos == std::string::npos) continue;
        size_t start = pos + key.size();
        if (start < line.size() && line[start] == '\"') {
            ++start;
            const size_t end = line.find('\"', start);
            return line.substr(start, end == std::string::npos ? end : end - start);
        }
        const size_t end = line.find_first_of(" \r\n", start);
        return line.substr(start, end == std::string::npos ? end : end - start);
    }
    return {};
}

bool HasAuthMethod(const std::string& methods, const std::string& wanted) {
    std::istringstream input(methods);
    std::string method;
    while (std::getline(input, method, ',')) {
        if (method == wanted) return true;
    }
    return false;
}

bool Authenticate(ControlConnection* connection,
                  const TorOnionServiceConfig& config,
                  std::string* method,
                  std::string* error) {
    TorControlReply protocol;
    if (!connection->Command("PROTOCOLINFO 1", &protocol, error) || !protocol.ok()) {
        if (error && error->empty()) *error = "Tor rejected PROTOCOLINFO";
        return false;
    }
    std::string auth_command;
    const std::string methods = ExtractValue(protocol, "METHODS=");
    const std::string cookie_file = ExtractValue(protocol, "COOKIEFILE=");
    if (HasAuthMethod(methods, "COOKIE") && !cookie_file.empty()) {
        std::vector<uint8_t> cookie;
        if (!ReadFileBinary(cookie_file, &cookie) || cookie.size() != 32) {
            if (error) *error = "could not read Tor 32-byte authentication cookie";
            return false;
        }
        auth_command = "AUTHENTICATE " + Hex(cookie);
        if (method) *method = "cookie";
    } else if (HasAuthMethod(methods, "HASHEDPASSWORD") &&
               !config.control_password.empty()) {
        auth_command = "AUTHENTICATE " + QuoteControlString(config.control_password);
        if (method) *method = "password";
    } else if (HasAuthMethod(methods, "NULL")) {
        auth_command = "AUTHENTICATE";
        if (method) *method = "null";
    } else {
        if (error) *error = "Tor requires SAFECOOKIE/password; no supported credential available";
        return false;
    }
    TorControlReply auth;
    if (!connection->Command(auth_command, &auth, error) || !auth.ok()) {
        if (error && error->empty()) *error = "Tor authentication failed";
        return false;
    }
    return true;
}

bool ValidServiceId(const std::string& id) {
    return id.size() == 56 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
    });
}

}  // namespace

bool ParseTorControlReply(const std::string& wire,
                          TorControlReply* reply,
                          std::string* error) {
    if (!reply) {
        if (error) *error = "null Tor control reply";
        return false;
    }
    reply->code = 0;
    reply->lines.clear();
    std::istringstream input(wire);
    std::string line;
    int expected_code = 0;
    bool complete = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 4 || !std::isdigit(static_cast<unsigned char>(line[0])) ||
            !std::isdigit(static_cast<unsigned char>(line[1])) ||
            !std::isdigit(static_cast<unsigned char>(line[2])) ||
            (line[3] != '-' && line[3] != ' ')) {
            if (error) *error = "malformed Tor control reply line";
            return false;
        }
        const int code = std::stoi(line.substr(0, 3));
        if (expected_code == 0) expected_code = code;
        if (code != expected_code) {
            if (error) *error = "mixed Tor control reply codes";
            return false;
        }
        reply->lines.push_back(line.substr(4));
        if (line[3] == ' ') {
            complete = true;
            reply->code = code;
            break;
        }
    }
    if (!complete) {
        if (error) *error = "incomplete Tor control reply";
        return false;
    }
    return true;
}

TorOnionService::TorOnionService(TorOnionServiceConfig config)
    : config_(std::move(config)) {
    status_.requested = true;
}

TorOnionService::~TorOnionService() { (void)Stop(); }

bool TorOnionService::SetError(const std::string& message) {
    status_.active = false;
    status_.message = message;
    return false;
}

bool TorOnionService::Start() {
    if (status_.active) return true;
    if (config_.control_port == 0 || config_.virtual_port == 0 ||
        config_.target_port == 0 || config_.private_key_path.empty()) {
        return SetError("invalid Tor onion-service configuration");
    }
    ControlConnection connection;
    std::string error;
    if (!connection.Connect(config_, &error)) return SetError(error);
    if (!Authenticate(&connection, config_, &status_.authentication, &error)) {
        return SetError(error);
    }

    std::string key = ReadTorPrivateKey(config_.private_key_path);
    const bool generated = key.empty();
    if (generated) key = "NEW:ED25519-V3";
    std::ostringstream command;
    command << "ADD_ONION " << key << " Flags=Detach Port="
            << config_.virtual_port << ',' << config_.target_host << ':'
            << config_.target_port;
    TorControlReply reply;
    if (!connection.Command(command.str(), &reply, &error) || !reply.ok()) {
        return SetError(error.empty() ? "Tor rejected ADD_ONION" : error);
    }
    const std::string service_id = ExtractValue(reply, "ServiceID=");
    if (!ValidServiceId(service_id)) {
        return SetError("Tor returned an invalid v3 ServiceID");
    }
    if (generated) {
        const std::string private_key = ExtractValue(reply, "PrivateKey=");
        if (private_key.rfind("ED25519-V3:", 0) != 0 ||
            !WriteTorPrivateKey(config_.private_key_path, private_key, &error)) {
            TorControlReply ignored;
            connection.Command("DEL_ONION " + service_id, &ignored, nullptr);
            return SetError(error.empty() ? "Tor did not return a persistent private key" : error);
        }
    }
    status_.service_id = service_id;
    status_.onion_address = service_id + ".onion";
    status_.active = true;
    status_.message = generated ? "created persistent Tor v3 onion service"
                                : "restored persistent Tor v3 onion service";
    return true;
}

bool TorOnionService::Stop() {
    if (!status_.active || status_.service_id.empty()) return true;
    ControlConnection connection;
    std::string error;
    std::string method;
    TorControlReply reply;
    if (connection.Connect(config_, &error) &&
        Authenticate(&connection, config_, &method, &error) &&
        connection.Command("DEL_ONION " + status_.service_id, &reply, &error) &&
        reply.ok()) {
        status_.message = "Tor v3 onion service removed";
        status_.active = false;
        return true;
    } else {
        status_.message = "Tor onion-service removal failed: " + error;
        // A detached onion mapping may still be live. Keep reporting active
        // until Tor confirms DEL_ONION; callers must not persist an off state.
        return false;
    }
}

}  // namespace dinero::network
