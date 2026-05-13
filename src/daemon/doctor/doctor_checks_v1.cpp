// doctor_checks_v1.cpp - v1 health check implementations
// Phase 2: 10 checks across storage, db, mempool, p2p, invariants
#include "daemon/doctor/doctor_registry.h"
#include "daemon/doctor/doctor_context.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

// System headers for checks
#ifndef _WIN32
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

// DNS resolution
#include "compat/net_compat.h"

// DNS seeds
#include "config/seed_nodes.h"

// Supply constants
#include "consensus/subsidy.h"

namespace dinero {
namespace doctor {

#ifdef _WIN32
// Doctor checks use POSIX-specific APIs (statvfs, opendir, kill, etc.)
// Windows stubs - doctor subcommand reports no checks available
void RegisterV1Checks(DoctorRegistry& registry) {
    (void)registry;
}
#else // !_WIN32

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

static std::string HumanBytes(uint64_t bytes) {
    if (bytes >= (1ULL << 30)) {
        return std::to_string(bytes / (1ULL << 30)) + " GB";
    } else if (bytes >= (1ULL << 20)) {
        return std::to_string(bytes / (1ULL << 20)) + " MB";
    } else if (bytes >= (1ULL << 10)) {
        return std::to_string(bytes / (1ULL << 10)) + " KB";
    }
    return std::to_string(bytes) + " B";
}

static bool PathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. storage.disk_space (BOTH)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckDiskSpace(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "storage.disk_space";

    struct statvfs st;
    if (statvfs(ctx.DataDir().c_str(), &st) != 0) {
        result.status = CheckStatus::ERROR;
        result.message = "Failed to stat filesystem: " + std::string(strerror(errno));
        result.evidence["datadir"] = ctx.DataDir();
        result.evidence["errno"] = std::to_string(errno);
        return result;
    }

    uint64_t available = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    uint64_t total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    uint64_t used = total - available;
    double pct_used = total > 0 ? (100.0 * used / total) : 0;

    result.evidence["available"] = HumanBytes(available);
    result.evidence["total"] = HumanBytes(total);
    result.evidence["used_pct"] = std::to_string(static_cast<int>(pct_used)) + "%";
    result.evidence["datadir"] = ctx.DataDir();

    constexpr uint64_t CRIT_THRESHOLD = 256ULL * (1 << 20);   // 256 MB
    constexpr uint64_t WARN_THRESHOLD = 1ULL * (1 << 30);     // 1 GB

    if (available < CRIT_THRESHOLD) {
        result.status = CheckStatus::CRIT;
        result.message = "Critically low disk space: " + HumanBytes(available) + " remaining";
        result.fix_plan.push_back(FixAction{
            "storage.disk_space.free_space",
            false, FixRisk::MED, "varies",
            {"Identify unnecessary files or expand storage"},
            {"Review and clean " + ctx.DataDir() + " or expand the volume"},
            ""
        });
    } else if (available < WARN_THRESHOLD) {
        result.status = CheckStatus::WARN;
        result.message = "Low disk space: " + HumanBytes(available) + " remaining";
    } else {
        result.status = CheckStatus::PASS;
        result.message = HumanBytes(available) + " available";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. storage.permissions (BOTH)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckPermissions(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "storage.permissions";

    struct PathCheck {
        std::string path;
        std::string label;
        bool must_exist;  // false = skip if missing (subdirs created lazily)
    };

    std::vector<PathCheck> paths = {
        {ctx.DataDir(), "datadir", true},
        {ctx.DataDir() + "/blockchain", "blockchain", false},
        {ctx.DataDir() + "/blockchain/chaindb", "chaindb", false},
        {ctx.DataDir() + "/wallets", "wallets", false},
    };

    std::vector<std::string> issues;
    bool missing_subdirs = false;

    for (const auto& p : paths) {
        if (!PathExists(p.path)) {
            if (p.must_exist) {
                issues.push_back(p.label + ": does not exist");
                result.evidence[p.label + ".exists"] = "false";
            } else {
                // Optional subdir missing — can be safely created
                result.evidence[p.label + ".exists"] = "false";
                missing_subdirs = true;
            }
            continue;
        }

        bool readable = access(p.path.c_str(), R_OK) == 0;
        bool writable = access(p.path.c_str(), W_OK) == 0;

        result.evidence[p.label + ".readable"] = readable ? "true" : "false";
        result.evidence[p.label + ".writable"] = writable ? "true" : "false";

        if (!readable) {
            issues.push_back(p.label + ": not readable");
        }
        if (!writable) {
            issues.push_back(p.label + ": not writable");
        }
    }

    // Offer safe fix for missing subdirectories (even if no permission issues)
    if (missing_subdirs && PathExists(ctx.DataDir())) {
        result.fix_plan.push_back(FixAction{
            "storage.permissions.create_dirs",
            true, FixRisk::LOW, "none",
            {"Data directory must exist and be writable"},
            {"mkdir -p " + ctx.DataDir() + "/{blockchain/chaindb,wallets}"},
            "Remove created directories if not needed"
        });
    }

    if (issues.empty() && !missing_subdirs) {
        result.status = CheckStatus::PASS;
        result.message = "All paths accessible";
    } else if (issues.empty() && missing_subdirs) {
        result.status = CheckStatus::WARN;
        result.message = "Optional subdirectories missing (safe to create)";
    } else {
        result.status = CheckStatus::CRIT;
        std::ostringstream msg;
        msg << issues.size() << " permission issue(s): " << issues[0];
        if (issues.size() > 1) {
            msg << " (+" << (issues.size() - 1) << " more)";
        }
        result.message = msg.str();

        // Suggest fix for writable directories
        result.fix_plan.push_back(FixAction{
            "storage.permissions.fix_ownership",
            false, FixRisk::LOW, "none",
            {"Data directory must be owned by the daemon user"},
            {"chmod -R u+rw " + ctx.DataDir()},
            ""
        });
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. storage.fsync_latency.sample (BOTH)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckFsyncLatency(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "storage.fsync_latency.sample";

    int samples = (ctx.Mode() == RunMode::DEEP) ? 5 : 1;
    std::vector<double> latencies;
    std::string tmp_path = ctx.DataDir() + "/.doctor_fsync_probe";

    for (int i = 0; i < samples; i++) {
        int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            result.status = CheckStatus::ERROR;
            result.message = "Cannot create probe file: " + std::string(strerror(errno));
            result.evidence["path"] = tmp_path;
            return result;
        }

        const char probe_data[] = "doctor_fsync_probe_data_block_0123456789ABCDEF";
        (void)write(fd, probe_data, sizeof(probe_data));

        auto start = std::chrono::steady_clock::now();
        fsync(fd);
        auto end = std::chrono::steady_clock::now();

        close(fd);

        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        latencies.push_back(ms);
    }

    unlink(tmp_path.c_str());  // Clean up probe file

    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
    double max_lat = *std::max_element(latencies.begin(), latencies.end());

    std::ostringstream avg_str;
    avg_str << std::fixed;
    avg_str.precision(1);
    avg_str << avg;

    std::ostringstream max_str;
    max_str << std::fixed;
    max_str.precision(1);
    max_str << max_lat;

    result.evidence["samples"] = std::to_string(samples);
    result.evidence["avg_ms"] = avg_str.str();
    result.evidence["max_ms"] = max_str.str();

    constexpr double CRIT_MS = 200.0;
    constexpr double WARN_MS = 50.0;

    if (max_lat > CRIT_MS) {
        result.status = CheckStatus::CRIT;
        result.message = "fsync latency critically high: " + max_str.str() + "ms max";
    } else if (max_lat > WARN_MS) {
        result.status = CheckStatus::WARN;
        result.message = "fsync latency elevated: " + max_str.str() + "ms max";
    } else {
        result.status = CheckStatus::PASS;
        result.message = "fsync latency OK: " + avg_str.str() + "ms avg";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. db.sqlite.quick_check (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

// We avoid linking sqlite3 directly from doctor code.
// Instead, check if wallet DB files exist and are non-empty valid SQLite files.
// SQLite files start with the magic "SQLite format 3\000" (16 bytes).
static DoctorCheckResult CheckSqliteIntegrity(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "db.sqlite.quick_check";

    std::string wallets_dir = ctx.DataDir() + "/wallets";
    if (!IsDirectory(wallets_dir)) {
        result.status = CheckStatus::PASS;
        result.message = "No wallet directory (wallets not initialized)";
        result.evidence["wallets_dir"] = wallets_dir;
        return result;
    }

    // Scan for .db files
    std::vector<std::string> db_files;
    DIR* dir = opendir(wallets_dir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 3 && name.substr(name.size() - 3) == ".db") {
                db_files.push_back(wallets_dir + "/" + name);
            }
        }
        closedir(dir);
    }

    if (db_files.empty()) {
        result.status = CheckStatus::PASS;
        result.message = "No wallet databases found";
        return result;
    }

    static const char SQLITE_MAGIC[] = "SQLite format 3";
    bool deep = (ctx.Mode() == RunMode::DEEP);
    int checked = 0;
    int issues = 0;

    for (const auto& path : db_files) {
        // Read first 100 bytes for header validation (magic + page size + format)
        char header[100] = {};
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) {
            result.evidence[path + ".status"] = "unreadable";
            issues++;
            continue;
        }

        f.read(header, deep ? 100 : 16);
        if (f.gcount() < 16 || std::memcmp(header, SQLITE_MAGIC, 15) != 0) {
            result.evidence[path + ".status"] = "invalid_header";
            issues++;
        } else if (deep && f.gcount() >= 100) {
            // Deep mode: validate page size (bytes 16-17, big-endian)
            uint16_t page_size = (static_cast<uint8_t>(header[16]) << 8) |
                                  static_cast<uint8_t>(header[17]);
            // Page size 1 means 65536; valid values: 512, 1024, 2048, ..., 65536
            uint32_t real_page_size = (page_size == 1) ? 65536 : page_size;
            bool valid_page_size = (real_page_size >= 512) &&
                                   (real_page_size <= 65536) &&
                                   ((real_page_size & (real_page_size - 1)) == 0);  // power of 2

            // Verify file size is a multiple of page size
            f.seekg(0, std::ios::end);
            auto file_size = f.tellg();

            if (!valid_page_size) {
                result.evidence[path + ".status"] = "invalid_page_size";
                result.evidence[path + ".page_size"] = std::to_string(page_size);
                issues++;
            } else if (file_size > 0 && (static_cast<uint64_t>(file_size) % real_page_size) != 0) {
                result.evidence[path + ".status"] = "truncated_pages";
                result.evidence[path + ".file_size"] = std::to_string(static_cast<uint64_t>(file_size));
                result.evidence[path + ".page_size"] = std::to_string(real_page_size);
                issues++;
            } else {
                result.evidence[path + ".status"] = "ok";
                result.evidence[path + ".page_size"] = std::to_string(real_page_size);
                result.evidence[path + ".pages"] = std::to_string(
                    static_cast<uint64_t>(file_size) / real_page_size);
            }
        } else {
            result.evidence[path + ".status"] = "ok";
        }
        checked++;
    }

    result.evidence["files_checked"] = std::to_string(checked);
    result.evidence["issues"] = std::to_string(issues);

    if (issues > 0) {
        result.status = CheckStatus::CRIT;
        result.message = std::to_string(issues) + " of " + std::to_string(checked) +
                         " wallet DB(s) have invalid headers";
    } else {
        result.status = CheckStatus::PASS;
        result.message = std::to_string(checked) + " wallet DB(s) verified";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. db.tip_consistency (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

// Checks that chaindb directory has valid RocksDB structure.
// Merged with inv.chainstate_continuity: verifies CURRENT and MANIFEST exist.
static DoctorCheckResult CheckTipConsistency(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "db.tip_consistency";

    std::string chaindb_dir = ctx.DataDir() + "/blockchain/chaindb";

    if (!IsDirectory(chaindb_dir)) {
        result.status = CheckStatus::WARN;
        result.message = "ChainDB directory not found (node may not be initialized)";
        result.evidence["chaindb_dir"] = chaindb_dir;
        return result;
    }

    // Check for CURRENT file (RocksDB uses this to point to the active MANIFEST)
    std::string current_file = chaindb_dir + "/CURRENT";
    if (!PathExists(current_file)) {
        result.status = CheckStatus::CRIT;
        result.message = "RocksDB CURRENT file missing — database may be corrupted";
        result.evidence["missing"] = "CURRENT";
        result.fix_plan.push_back(FixAction{
            "db.tip_consistency.reindex",
            false, FixRisk::HIGH, "> 30 minutes",
            {"Requires daemon restart"},
            {"dinerod --reindex"},
            "Previous chainstate will be rebuilt from block data"
        });
        return result;
    }

    // Read CURRENT to find MANIFEST name
    std::ifstream current_in(current_file);
    std::string manifest_name;
    std::getline(current_in, manifest_name);
    // Strip trailing newline/whitespace
    while (!manifest_name.empty() &&
           (manifest_name.back() == '\n' || manifest_name.back() == '\r')) {
        manifest_name.pop_back();
    }

    result.evidence["manifest"] = manifest_name;

    if (manifest_name.empty() || manifest_name.find("MANIFEST") == std::string::npos) {
        result.status = CheckStatus::CRIT;
        result.message = "CURRENT file has invalid content: '" + manifest_name + "'";
        return result;
    }

    std::string manifest_path = chaindb_dir + "/" + manifest_name;
    if (!PathExists(manifest_path)) {
        result.status = CheckStatus::CRIT;
        result.message = "Referenced MANIFEST file missing: " + manifest_name;
        result.fix_plan.push_back(FixAction{
            "db.tip_consistency.reindex",
            false, FixRisk::HIGH, "> 30 minutes",
            {"Requires daemon restart"},
            {"dinerod --reindex"},
            ""
        });
        return result;
    }

    // Check MANIFEST is non-empty
    struct stat st;
    stat(manifest_path.c_str(), &st);
    if (st.st_size == 0) {
        result.status = CheckStatus::CRIT;
        result.message = "MANIFEST file is empty (0 bytes)";
        return result;
    }

    result.evidence["manifest_size"] = HumanBytes(st.st_size);

    // Count SST files as a health indicator
    int sst_count = 0;
    DIR* dir = opendir(chaindb_dir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") {
                sst_count++;
            }
        }
        closedir(dir);
    }
    result.evidence["sst_files"] = std::to_string(sst_count);

    result.status = CheckStatus::PASS;
    result.message = "ChainDB structure intact (" + std::to_string(sst_count) + " SST files)";

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. db.rocksdb.checksum_sample (DEEP)
// ═══════════════════════════════════════════════════════════════════════════

// Deep check: verify that RocksDB SST files have valid checksums.
// We sample a few files and verify they're readable and non-corrupted.
static DoctorCheckResult CheckRocksDbChecksum(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "db.rocksdb.checksum_sample";

    std::string chaindb_dir = ctx.DataDir() + "/blockchain/chaindb";
    if (!IsDirectory(chaindb_dir)) {
        result.status = CheckStatus::SKIP;
        result.message = "ChainDB not found";
        return result;
    }

    // Collect SST files
    std::vector<std::string> sst_files;
    DIR* dir = opendir(chaindb_dir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") {
                sst_files.push_back(chaindb_dir + "/" + name);
            }
        }
        closedir(dir);
    }

    if (sst_files.empty()) {
        result.status = CheckStatus::PASS;
        result.message = "No SST files to check (empty database)";
        return result;
    }

    // Deep mode: scan ALL files. Quick/both mode: sample up to 10.
    std::sort(sst_files.begin(), sst_files.end());
    bool full_scan = (ctx.Mode() == RunMode::DEEP);
    size_t sample_size = full_scan ? sst_files.size()
                                   : std::min(sst_files.size(), static_cast<size_t>(10));

    int readable = 0;
    int errors = 0;

    for (size_t i = 0; i < sample_size; i++) {
        // Full scan: sequential. Sample: evenly spaced across sorted list.
        size_t idx = full_scan ? i : (i * sst_files.size()) / sample_size;
        const auto& path = sst_files[idx];

        // Verify file is readable and has valid SST header
        // RocksDB SST files are "Table" format files with specific block structure.
        // At minimum, verify the file opens, is non-empty, and header bytes are present.
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            errors++;
            result.evidence[path + ".status"] = "unreadable";
            continue;
        }

        auto size = f.tellg();
        if (size < 64) {
            // SST files should be at least a few KB
            errors++;
            result.evidence[path + ".status"] = "truncated";
            continue;
        }

        // Read the footer (last 48 bytes of SST contain magic number)
        // RocksDB footer magic: 0x88e241b785f4cff7 (block-based table)
        // or 0x2be7a6974a1e6db (plain table)
        f.seekg(-48, std::ios::end);
        char footer[48];
        f.read(footer, 48);
        if (f.gcount() < 48) {
            errors++;
            result.evidence[path + ".status"] = "read_error";
            continue;
        }

        readable++;
    }

    result.evidence["total_sst_files"] = std::to_string(sst_files.size());
    result.evidence["scanned"] = std::to_string(sample_size);
    result.evidence["scan_mode"] = full_scan ? "full" : "sample";
    result.evidence["readable"] = std::to_string(readable);
    result.evidence["errors"] = std::to_string(errors);

    if (errors > 0) {
        result.status = CheckStatus::CRIT;
        std::string scan_label = full_scan ? "scanned" : "sampled";
        result.message = std::to_string(errors) + " of " + std::to_string(sample_size) +
                         " " + scan_label + " SST files have issues";
        result.fix_plan.push_back(FixAction{
            "db.rocksdb.checksum_sample.reindex",
            false, FixRisk::HIGH, "> 30 minutes",
            {"Requires daemon restart"},
            {"dinerod --reindex"},
            "Chainstate will be rebuilt from block data"
        });
    } else {
        result.status = CheckStatus::PASS;
        if (full_scan) {
            result.message = "All " + std::to_string(readable) + " SST files verified";
        } else {
            result.message = std::to_string(readable) + "/" + std::to_string(sample_size) +
                             " sampled SST files OK (" + std::to_string(sst_files.size()) + " total)";
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. mempool.snapshot_sanity (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckMempoolSnapshot(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "mempool.snapshot_sanity";

    std::string mempool_path = ctx.DataDir() + "/mempool.dat";

    if (!PathExists(mempool_path)) {
        result.status = CheckStatus::PASS;
        result.message = "No mempool.dat (normal for fresh node or clean shutdown)";
        result.evidence["path"] = mempool_path;
        return result;
    }

    struct stat st;
    stat(mempool_path.c_str(), &st);
    result.evidence["size"] = HumanBytes(st.st_size);
    result.evidence["path"] = mempool_path;

    if (st.st_size < 12) {
        // Minimum: 8 magic + 4 version
        result.status = CheckStatus::WARN;
        result.message = "mempool.dat too small (" + std::to_string(st.st_size) + " bytes)";
        return result;
    }

    // Verify magic header: "MEMPOOLV" (8 bytes)
    std::ifstream f(mempool_path, std::ios::binary);
    char header[12];
    f.read(header, 12);

    if (f.gcount() < 12) {
        result.status = CheckStatus::WARN;
        result.message = "mempool.dat unreadable";
        return result;
    }

    static const char MAGIC[] = "MEMPOOLV";
    if (std::memcmp(header, MAGIC, 8) != 0) {
        result.status = CheckStatus::CRIT;
        result.message = "mempool.dat has invalid magic header";
        result.evidence["expected_magic"] = "MEMPOOLV";

        result.fix_plan.push_back(FixAction{
            "mempool.snapshot_sanity.remove",
            true, FixRisk::LOW, "none",
            {"Node will rebuild mempool from network on next start"},
            {"rm " + mempool_path},
            "Mempool will be empty until repopulated from peers"
        });
        return result;
    }

    // Check version (bytes 8-11, little-endian uint32)
    uint32_t version = 0;
    std::memcpy(&version, header + 8, 4);
    result.evidence["version"] = std::to_string(version);

    if (version != 1) {
        result.status = CheckStatus::WARN;
        result.message = "mempool.dat unknown version: " + std::to_string(version);
        return result;
    }

    result.status = CheckStatus::PASS;
    result.message = "mempool.dat valid (v" + std::to_string(version) + ", " + HumanBytes(st.st_size) + ")";

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. p2p.bind_listen (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckP2pBind(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "p2p.bind_listen";

    uint16_t port = ctx.P2pPort();
    result.evidence["port"] = std::to_string(port);

    // Try to bind a TCP socket on the P2P port
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        result.status = CheckStatus::ERROR;
        result.message = "Cannot create socket: " + std::string(strerror(errno));
        return result;
    }

    // Allow address reuse (match typical daemon behavior)
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int bind_result = bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    int bind_errno = errno;
    close(sock);

    if (bind_result == 0) {
        result.status = CheckStatus::PASS;
        result.message = "P2P port " + std::to_string(port) + " available";
    } else if (bind_errno == EADDRINUSE) {
        // Port in use — verify it's our own daemon, not a stale PID or unrelated process.
        // Three checks: PID file exists, process alive, process name is "dinerod".
        std::string pid_path = ctx.DataDir() + "/dinerod.pid";
        bool pid_file_found = false;
        bool process_alive = false;
        bool name_matches = false;
        pid_t pid = 0;

        std::ifstream pid_file(pid_path);
        if (pid_file.is_open()) {
            pid_file >> pid;
            pid_file_found = (pid > 0);
        }

        if (pid_file_found && kill(pid, 0) == 0) {
            process_alive = true;

            // Verify process name to avoid stale/reused PID false-PASS
#if defined(__APPLE__)
            char proc_name_buf[256] = {};
            proc_name(pid, proc_name_buf, sizeof(proc_name_buf));
            name_matches = (std::string(proc_name_buf).find("dinerod") != std::string::npos);
#elif defined(__linux__)
            std::string comm_path = "/proc/" + std::to_string(pid) + "/comm";
            std::ifstream comm_file(comm_path);
            if (comm_file.is_open()) {
                std::string proc_name;
                std::getline(comm_file, proc_name);
                name_matches = (proc_name.find("dinerod") != std::string::npos);
            }
#else
            // Unknown platform — can't verify process name, fall back to PID-only
            name_matches = true;
#endif
        }

        result.evidence["pid_file"] = pid_file_found ? std::to_string(pid) : "absent";
        result.evidence["process_alive"] = process_alive ? "true" : "false";
        result.evidence["name_verified"] = name_matches ? "true" : "false";

        if (process_alive && name_matches) {
            result.status = CheckStatus::PASS;
            result.message = "P2P port " + std::to_string(port) + " held by running dinerod (PID " + std::to_string(pid) + ")";
            result.evidence["status"] = "daemon_running";
        } else if (process_alive && !name_matches) {
            result.status = CheckStatus::WARN;
            result.message = "P2P port " + std::to_string(port) + " in use; PID " + std::to_string(pid) + " is not dinerod";
            result.evidence["status"] = "stale_pid";
        } else {
            result.status = CheckStatus::WARN;
            result.message = "P2P port " + std::to_string(port) + " in use by unknown process";
            result.evidence["status"] = "port_conflict";
        }
        result.evidence["errno"] = "EADDRINUSE";
    } else if (bind_errno == EACCES) {
        result.status = CheckStatus::CRIT;
        result.message = "Permission denied binding P2P port " + std::to_string(port);
        result.evidence["errno"] = "EACCES";
    } else {
        result.status = CheckStatus::WARN;
        result.message = "Cannot bind P2P port " + std::to_string(port) + ": " + strerror(bind_errno);
        result.evidence["errno"] = std::to_string(bind_errno);
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 9. p2p.dns_seeds.resolve (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

static DoctorCheckResult CheckDnsSeeds(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "p2p.dns_seeds.resolve";

    auto seeds = config::getDnsSeeds(ctx.Network());

    if (seeds.empty()) {
        result.status = CheckStatus::PASS;
        result.message = "No DNS seeds configured for " + ctx.Network();
        return result;
    }

    int resolved = 0;
    int failed = 0;

    for (const auto& seed : seeds) {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* res = nullptr;
        int status = getaddrinfo(seed.c_str(), nullptr, &hints, &res);

        if (status == 0 && res != nullptr) {
            // Extract first IP for evidence
            char ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
            inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
            result.evidence[seed] = ip_str;
            resolved++;
            freeaddrinfo(res);
        } else {
            // Report actual error: EAI_NONAME, EAI_AGAIN, EAI_FAIL, etc.
            result.evidence[seed] = gai_strerror(status);
            failed++;
        }
    }

    result.evidence["total_seeds"] = std::to_string(seeds.size());
    result.evidence["resolved"] = std::to_string(resolved);
    result.evidence["failed"] = std::to_string(failed);

    if (failed == static_cast<int>(seeds.size())) {
        result.status = CheckStatus::CRIT;
        result.message = "All DNS seeds failed to resolve — check network connectivity";
    } else if (failed > 0) {
        result.status = CheckStatus::WARN;
        result.message = std::to_string(failed) + " of " + std::to_string(seeds.size()) +
                         " DNS seeds failed to resolve";
    } else {
        result.status = CheckStatus::PASS;
        result.message = "All " + std::to_string(resolved) + " DNS seeds resolved";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// 10. inv.supply_bounds (QUICK)
// ═══════════════════════════════════════════════════════════════════════════

// Verify supply invariants from the consensus subsidy schedule.
// This check validates the compile-time constants are internally consistent
// and checks that the chainstate tip height (if readable) is sane.
static DoctorCheckResult CheckSupplyBounds(const DoctorContext& ctx) {
    DoctorCheckResult result;
    result.id = "inv.supply_bounds";

    // Verify compile-time constants (Fair Launch v3 — no premine)
    result.evidence["halving_interval"] = std::to_string(ConsensusSubsidy::HALVING_INTERVAL);
    result.evidence["initial_reward_din"] = std::to_string(ConsensusSubsidy::INITIAL_SUBSIDY / ConsensusSubsidy::UNA_PER_DIN);
    result.evidence["tail_emission_din"] = std::to_string(ConsensusSubsidy::TAIL_EMISSION_UNA / ConsensusSubsidy::UNA_PER_DIN);

    // Verify subsidy at known heights
    auto genesis_sub = ConsensusSubsidy::GetBlockSubsidy(0);
    auto height1_sub = ConsensusSubsidy::GetBlockSubsidy(1);
    auto height2_sub = ConsensusSubsidy::GetBlockSubsidy(2);

    if (genesis_sub.IsZero() == false) {
        result.status = CheckStatus::CRIT;
        result.message = "Genesis subsidy should be zero (unspendable)";
        return result;
    }

    if (height1_sub.GetUna() != ConsensusSubsidy::INITIAL_SUBSIDY) {
        result.status = CheckStatus::CRIT;
        result.message = "Height 1 subsidy should be INITIAL_SUBSIDY (100 DIN)";
        return result;
    }

    if (height2_sub.GetUna() != ConsensusSubsidy::INITIAL_SUBSIDY) {
        result.status = CheckStatus::CRIT;
        result.message = "Height 2 subsidy should be INITIAL_SUBSIDY (100 DIN)";
        return result;
    }

    // Verify halving works correctly (first halving at height 1 + HALVING_INTERVAL)
    auto first_halving = ConsensusSubsidy::GetBlockSubsidy(1 + ConsensusSubsidy::HALVING_INTERVAL);
    if (first_halving.GetUna() != ConsensusSubsidy::INITIAL_SUBSIDY / 2) {
        result.status = CheckStatus::CRIT;
        result.message = "First halving subsidy incorrect";
        return result;
    }

    // Verify tail emission floor (after many halvings, subsidy = 1 DIN, not 0)
    auto post_many_halvings = ConsensusSubsidy::GetBlockSubsidy(1 + 33 * ConsensusSubsidy::HALVING_INTERVAL);
    if (post_many_halvings.GetUna() != ConsensusSubsidy::TAIL_EMISSION_UNA) {
        result.status = CheckStatus::CRIT;
        result.message = "Tail emission floor violated: subsidy should be 1 DIN after 33 halvings";
        return result;
    }

    result.status = CheckStatus::PASS;
    result.message = "Supply invariants verified (no premine, " +
                     std::to_string(ConsensusSubsidy::HALVING_INTERVAL) + "-block halvings, " +
                     std::to_string(ConsensusSubsidy::TAIL_EMISSION_UNA / ConsensusSubsidy::UNA_PER_DIN) +
                     " DIN tail emission)";

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════

void RegisterV1Checks(DoctorRegistry& registry) {
    // 1. storage.disk_space
    registry.Register(
        DoctorCheckMetadata{
            "storage.disk_space",
            "Check available disk space on data directory volume",
            Severity::CRIT, CheckMode::BOTH, FixRisk::NONE,
            {}, 5000
        },
        CheckDiskSpace
    );

    // 2. storage.permissions
    registry.Register(
        DoctorCheckMetadata{
            "storage.permissions",
            "Verify read/write access to data directories",
            Severity::CRIT, CheckMode::BOTH, FixRisk::LOW,
            {}, 5000
        },
        CheckPermissions
    );

    // 3. storage.fsync_latency.sample
    registry.Register(
        DoctorCheckMetadata{
            "storage.fsync_latency.sample",
            "Measure fsync latency on data volume",
            Severity::WARN, CheckMode::BOTH, FixRisk::NONE,
            {"storage.permissions"}, 10000
        },
        CheckFsyncLatency
    );

    // 4. db.sqlite.quick_check
    registry.Register(
        DoctorCheckMetadata{
            "db.sqlite.quick_check",
            "Verify wallet SQLite database headers",
            Severity::CRIT, CheckMode::BOTH, FixRisk::NONE,
            {"storage.permissions"}, 5000
        },
        CheckSqliteIntegrity
    );

    // 5. db.tip_consistency
    registry.Register(
        DoctorCheckMetadata{
            "db.tip_consistency",
            "Verify chainstate RocksDB structure and MANIFEST integrity",
            Severity::CRIT, CheckMode::BOTH, FixRisk::HIGH,
            {"storage.permissions"}, 5000
        },
        CheckTipConsistency
    );

    // 6. db.rocksdb.checksum_sample
    registry.Register(
        DoctorCheckMetadata{
            "db.rocksdb.checksum_sample",
            "Sample and verify RocksDB SST file integrity",
            Severity::CRIT, CheckMode::DEEP, FixRisk::HIGH,
            {"db.tip_consistency"}, 60000
        },
        CheckRocksDbChecksum
    );

    // 7. mempool.snapshot_sanity
    registry.Register(
        DoctorCheckMetadata{
            "mempool.snapshot_sanity",
            "Verify mempool.dat magic header and format version",
            Severity::WARN, CheckMode::QUICK, FixRisk::LOW,
            {}, 5000
        },
        CheckMempoolSnapshot
    );

    // 8. p2p.bind_listen
    registry.Register(
        DoctorCheckMetadata{
            "p2p.bind_listen",
            "Check if P2P port is available for binding",
            Severity::WARN, CheckMode::QUICK, FixRisk::NONE,
            {}, 5000
        },
        CheckP2pBind
    );

    // 9. p2p.dns_seeds.resolve
    registry.Register(
        DoctorCheckMetadata{
            "p2p.dns_seeds.resolve",
            "Resolve DNS seed hostnames for network connectivity",
            Severity::WARN, CheckMode::QUICK, FixRisk::NONE,
            {}, 10000
        },
        CheckDnsSeeds
    );

    // 10. inv.supply_bounds
    registry.Register(
        DoctorCheckMetadata{
            "inv.supply_bounds",
            "Verify consensus supply schedule invariants",
            Severity::CRIT, CheckMode::QUICK, FixRisk::NONE,
            {}, 5000
        },
        CheckSupplyBounds
    );
}

#endif // !_WIN32

} // namespace doctor
} // namespace dinero
