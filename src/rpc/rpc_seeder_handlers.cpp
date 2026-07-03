// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/rpc_seeder_handlers.h"

#include "config/seed_nodes.h"
#include "daemon/services/config_service.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dinero::rpc {

namespace {

constexpr const char* kSchema = "din.rpc.v1";
constexpr int kDefaultSeederBatch = 8;
constexpr int kDefaultSeederCyclePauseSeconds = 30;

din::Json Error(int code, const std::string& message) {
    din::Json out;
    out["error"]["code"] = code;
    out["error"]["message"] = message;
    return out;
}

std::string JsonString(const din::Json& obj, const char* key,
                       const std::string& fallback = {}) {
    return obj.isObject() && obj.isMember(key) && obj[key].isString()
        ? obj[key].asString()
        : fallback;
}

int JsonInt(const din::Json& obj, const char* key, int fallback) {
    return obj.isObject() && obj.isMember(key) && obj[key].isInt()
        ? obj[key].asInt()
        : fallback;
}

std::vector<std::string> JsonStringArray(const din::Json& obj, const char* key) {
    std::vector<std::string> out;
    if (!obj.isObject() || !obj.isMember(key) || !obj[key].isArray()) return out;
    for (const auto& item : obj[key]) {
        if (item.isString() && !item.asString().empty()) {
            out.push_back(item.asString());
        }
    }
    return out;
}

std::string JoinCsv(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) oss << ',';
        oss << items[i];
    }
    return oss.str();
}

std::vector<std::string> DefaultBootstrap(dinero::ConfigService* config) {
    const int port = config ? config->P2PPort() : 20999;
    std::vector<std::string> out;
    out.reserve(dinero::config::MAINNET_SEED_IPS.size());
    for (const auto& seed : dinero::config::MAINNET_SEED_IPS) {
        out.push_back(seed.hostname + ":" + std::to_string(port));
    }
    return out;
}

std::filesystem::path SeederDir(dinero::ConfigService* config) {
    const std::string datadir = config ? config->DataDir() : std::string(".");
    return std::filesystem::path(datadir) / "seeder";
}

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

#ifndef _WIN32
using ProcessId = pid_t;
constexpr ProcessId kNoProcess = -1;
#else
using ProcessId = DWORD;
constexpr ProcessId kNoProcess = 0;
#endif

struct SeederRunConfig {
    std::string binary_path;
    std::vector<std::string> bootstrap;
    std::string state_path;
    std::string output_path;
    std::string log_path;
    int batch = kDefaultSeederBatch;
    int cycle_pause_seconds = kDefaultSeederCyclePauseSeconds;
};

class SeederProcessController {
public:
    ~SeederProcessController() { StopLockedForDestruction(); }

    din::Json Status() {
        std::lock_guard<std::mutex> lock(mu_);
        ReapIfExitedLocked();
        return StatusLocked();
    }

    din::Json Start(const SeederRunConfig& cfg) {
        std::lock_guard<std::mutex> lock(mu_);
        ReapIfExitedLocked();
        if (running_) return StatusLocked();
        if (cfg.binary_path.empty()) {
            last_error_ = "binary path is required";
            return Error(-8, last_error_);
        }
        std::error_code exists_ec;
        if (!std::filesystem::is_regular_file(cfg.binary_path, exists_ec)) {
            last_error_ = "seeder binary not found: " + cfg.binary_path;
            return Error(-8, last_error_);
        }
        if (cfg.bootstrap.empty()) {
            last_error_ = "at least one bootstrap peer is required";
            return Error(-8, last_error_);
        }

        std::filesystem::create_directories(std::filesystem::path(cfg.state_path).parent_path());
        std::filesystem::create_directories(std::filesystem::path(cfg.output_path).parent_path());
        std::filesystem::create_directories(std::filesystem::path(cfg.log_path).parent_path());

        std::vector<std::string> args{
            cfg.binary_path,
            "--bootstrap=" + JoinCsv(cfg.bootstrap),
            "--state=" + cfg.state_path,
            "--output=" + cfg.output_path,
            "--batch=" + std::to_string(cfg.batch),
            "--cycle-pause=" + std::to_string(cfg.cycle_pause_seconds),
        };

        std::string error;
        ProcessId pid = kNoProcess;
        if (!Spawn(args, cfg.log_path, pid, error)) {
            last_error_ = error;
            return Error(-32000, last_error_);
        }

        running_ = true;
        stopping_ = false;
        pid_ = pid;
        started_at_unix_ = NowUnix();
        last_exit_code_ = 0;
        last_error_.clear();
        cfg_ = cfg;
        return StatusLocked();
    }

    din::Json Stop() {
        std::lock_guard<std::mutex> lock(mu_);
        ReapIfExitedLocked();
        if (!running_) return StatusLocked();
        stopping_ = true;
        std::string error;
        if (!Terminate(pid_, error)) {
            last_error_ = error;
            return Error(-32001, last_error_);
        }
        WaitForExitLocked(kGracefulStopWait);
        if (running_) {
            if (!ForceTerminate(pid_, error)) {
                last_error_ = error;
                return Error(-32001, last_error_);
            }
            WaitForExitLocked(kForcedStopWait);
        }
        if (running_) {
            last_error_ = "seeder process did not exit after SIGTERM/SIGKILL";
            return Error(-32001, last_error_);
        }
        return StatusLocked();
    }

private:
    static constexpr auto kGracefulStopWait = std::chrono::seconds(2);
    static constexpr auto kForcedStopWait = std::chrono::seconds(1);

    std::mutex mu_;
    bool running_{false};
    bool stopping_{false};
    ProcessId pid_{kNoProcess};
    int64_t started_at_unix_{0};
    int last_exit_code_{0};
    std::string last_error_;
    SeederRunConfig cfg_;
#ifdef _WIN32
    HANDLE process_handle_{nullptr};
#endif

    din::Json StatusLocked() const {
        din::Json out;
        out["rpc_schema"] = kSchema;
        out["running"] = running_;
        out["stopping"] = stopping_;
        out["pid"] = running_ ? static_cast<Json::UInt64>(pid_) : 0;
        out["started_at"] = static_cast<Json::Int64>(started_at_unix_);
        out["uptime_seconds"] = running_ && started_at_unix_ > 0
            ? static_cast<Json::Int64>(NowUnix() - started_at_unix_)
            : 0;
        out["binary"] = cfg_.binary_path;
        out["state_path"] = cfg_.state_path;
        out["output_path"] = cfg_.output_path;
        out["log_path"] = cfg_.log_path;
        out["last_exit_code"] = last_exit_code_;
        out["last_error"] = last_error_;
        din::Json bootstrap(Json::arrayValue);
        for (const auto& peer : cfg_.bootstrap) bootstrap.append(peer);
        out["bootstrap"] = bootstrap;
        return out;
    }

    void MarkStoppedLocked(int exit_code) {
        running_ = false;
        stopping_ = false;
        pid_ = kNoProcess;
        started_at_unix_ = 0;
        last_exit_code_ = exit_code;
#ifdef _WIN32
        if (process_handle_) {
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
#endif
    }

    void ReapIfExitedLocked() {
        if (!running_) return;
#ifndef _WIN32
        int status = 0;
        const pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            int exit_code = 0;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            }
            MarkStoppedLocked(exit_code);
        }
#else
        if (!process_handle_) return;
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(process_handle_, &code) && code != STILL_ACTIVE) {
            MarkStoppedLocked(static_cast<int>(code));
        }
#endif
    }

    void WaitForExitLocked(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            ReapIfExitedLocked();
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ReapIfExitedLocked();
    }

    void StopLockedForDestruction() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return;
        std::string ignored;
        Terminate(pid_, ignored);
        WaitForExitLocked(kGracefulStopWait);
        if (running_) {
            ForceTerminate(pid_, ignored);
            WaitForExitLocked(kForcedStopWait);
        }
    }

    bool Spawn(const std::vector<std::string>& args, const std::string& log_path,
               ProcessId& pid_out, std::string& error) {
#ifndef _WIN32
        pid_t child = fork();
        if (child < 0) {
            error = "fork failed";
            return false;
        }
        if (child == 0) {
            int fd = open(log_path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            execv(args.front().c_str(), argv.data());
            _exit(127);
        }
        pid_out = child;
        return true;
#else
        auto widen = [](const std::string& s) {
            if (s.empty()) return std::wstring();
            const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring out(static_cast<size_t>(len - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
            return out;
        };
        auto quote = [](const std::wstring& s) {
            std::wstring out = L"\"";
            for (wchar_t c : s) {
                if (c == L'"') out += L'\\';
                out += c;
            }
            out += L"\"";
            return out;
        };
        std::wstring cmd;
        for (const auto& arg : args) {
            if (!cmd.empty()) cmd += L' ';
            cmd += quote(widen(arg));
        }
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');
        if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            error = "CreateProcessW failed";
            return false;
        }
        CloseHandle(pi.hThread);
        process_handle_ = pi.hProcess;
        pid_out = pi.dwProcessId;
        return true;
#endif
    }

    bool Terminate(ProcessId pid, std::string& error) {
        if (pid == kNoProcess) return true;
#ifndef _WIN32
        if (kill(pid, SIGTERM) != 0) {
            error = "SIGTERM failed";
            return false;
        }
        return true;
#else
        if (!process_handle_) return true;
        if (!TerminateProcess(process_handle_, 0)) {
            error = "TerminateProcess failed";
            return false;
        }
        return true;
#endif
    }

    bool ForceTerminate(ProcessId pid, std::string& error) {
        if (pid == kNoProcess) return true;
#ifndef _WIN32
        if (kill(pid, SIGKILL) != 0) {
            error = "SIGKILL failed";
            return false;
        }
        return true;
#else
        if (!process_handle_) return true;
        if (!TerminateProcess(process_handle_, 1)) {
            error = "TerminateProcess failed";
            return false;
        }
        return true;
#endif
    }
};

SeederProcessController& SeederController() {
    static SeederProcessController controller;
    return controller;
}

SeederRunConfig BuildRunConfig(dinero::ConfigService* config,
                               const din::Json& params) {
    const auto dir = SeederDir(config);
    SeederRunConfig cfg;
    // SECURITY (RCE fix): NEVER honor a caller-supplied "binary" path. Previously
    // `params["binary"]` was used as the execv() target, letting any authenticated
    // (or even --rpc-readonly) RPC caller run an arbitrary binary as the daemon
    // user. Only the operator-configured `seeder.binary` is permitted; the
    // caller-supplied field is ignored entirely.
    cfg.binary_path = config ? config->GetString("seeder.binary", "") : "";
    cfg.bootstrap = JsonStringArray(params, "bootstrap");
    if (cfg.bootstrap.empty()) cfg.bootstrap = DefaultBootstrap(config);
    cfg.state_path = JsonString(params, "state_path",
        (dir / "peers.state").string());
    cfg.output_path = JsonString(params, "output_path",
        (dir / "seeds_observed.txt").string());
    cfg.log_path = JsonString(params, "log_path",
        (dir / "seeder.log").string());
    cfg.batch = JsonInt(params, "batch", kDefaultSeederBatch);
    cfg.cycle_pause_seconds =
        JsonInt(params, "cycle_pause_seconds", kDefaultSeederCyclePauseSeconds);
    return cfg;
}

}  // namespace

din::Json HandleSeederStatus(dinero::ConfigService* /*config*/) {
    return SeederController().Status();
}

din::Json HandleSeederStart(dinero::ConfigService* config,
                            const din::Json& params) {
    if (!params.isObject()) {
        return Error(-8, "seeder.start expects an object parameter");
    }
    return SeederController().Start(BuildRunConfig(config, params));
}

din::Json HandleSeederStop(dinero::ConfigService* config) {
    (void)config;
    return SeederController().Stop();
}

}  // namespace dinero::rpc
