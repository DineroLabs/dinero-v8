#include "nodecore/maintenance_journal.h"
#include "nodecore/nodecore_ffi.h"
#include "crypto/sha256.h"

#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace dinero::nodecore {
namespace {
namespace fs = std::filesystem;
constexpr const char* kControl = ".nodecore-maintenance-v1";
constexpr size_t kMaxRecord = 16384;
constexpr uint64_t kMaxInspectionBytes = 64ULL * 1024 * 1024;
constexpr size_t kMaxInspectionEntries = 4096;

struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }

fs::path Target(const std::string& input) {
    const fs::path path(input);
    if (input.empty() || input.size() > 4096 || !path.is_absolute()) Fail("invalid datadir");
    // Resolves existing aliases, including /var on macOS, without creating a
    // missing target. Startup must inspect the sibling barrier before Init.
    const auto canonical = fs::weakly_canonical(path);
    if (canonical == canonical.root_path() || canonical.filename() == kControl) Fail("invalid target");
    return canonical;
}

struct stat Stat(const fs::path& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) Fail("stat failed");
    return st;
}

std::string Hex(const unsigned char* data, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 15]);
    }
    return result;
}

bool IsHex(const Json::Value& value, size_t size) {
    if (!value.isString() || value.asString().size() != size) return false;
    const auto text = value.asString();
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void Number(crypto::CSHA256& hash, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    hash.Write(bytes, sizeof(bytes));
}

std::string Inventory(const fs::path& root) {
    if (!S_ISDIR(Stat(root).st_mode)) Fail("inspection needs an existing directory");
    std::vector<fs::path> paths{root};
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (paths.size() >= kMaxInspectionEntries) Fail("inspection entry limit");
        const auto st = Stat(entry.path());
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) Fail("inspection rejects special files and symlinks");
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    crypto::CSHA256 hash;
    uint64_t bytes = 0;
    for (const auto& path : paths) {
        const auto st = Stat(path);
        const auto name = path.lexically_relative(root).generic_string();
        Number(hash, name.size());
        hash.Write(reinterpret_cast<const unsigned char*>(name.data()), name.size());
        Number(hash, st.st_mode);
        if (S_ISDIR(st.st_mode)) continue;
        if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<uint64_t>(st.st_size) > kMaxInspectionBytes - bytes) Fail("inspection byte limit or file type");
        bytes += static_cast<uint64_t>(st.st_size);
        Number(hash, st.st_size);
        Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (file.value < 0) Fail("inspection open failed");
        struct stat opened{};
        if (::fstat(file.value, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_dev != st.st_dev ||
            opened.st_ino != st.st_ino || opened.st_size != st.st_size) Fail("inspection identity changed");
        std::array<unsigned char, 32768> buffer{};
        uint64_t read_bytes = 0;
        while (true) {
            const auto n = ::read(file.value, buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) Fail("inspection read failed");
            if (n == 0) break;
            read_bytes += static_cast<uint64_t>(n);
            if (read_bytes > static_cast<uint64_t>(st.st_size)) Fail("inspection file grew");
            hash.Write(buffer.data(), static_cast<size_t>(n));
        }
        if (read_bytes != static_cast<uint64_t>(st.st_size)) Fail("inspection short read");
        const auto after = Stat(path);
        if (after.st_dev != st.st_dev || after.st_ino != st.st_ino || after.st_size != st.st_size ||
            after.st_mtime != st.st_mtime || after.st_ctime != st.st_ctime) Fail("inspection changed during read");
    }
    unsigned char digest[32];
    hash.Finalize(digest);
    return Hex(digest, sizeof(digest));
}

void ValidateRecord(const Json::Value& record) {
    static const std::vector<std::string> fields = {"format", "inventory", "operation_id", "parent_device",
        "parent_inode", "phase", "plan", "target", "target_device", "target_inode"};
    if (!record.isObject() || record.getMemberNames() != fields || !record["format"].isUInt() ||
        record["format"].asUInt() != 1 || record["plan"] != MaintenanceJournal::kInspection ||
        !record["target"].isString() || Target(record["target"].asString()).string() != record["target"].asString() ||
        !IsHex(record["inventory"], 64) || !IsHex(record["operation_id"], 32) ||
        (record["phase"] != "prepared" && record["phase"] != "completed")) Fail("unknown or malformed intent");
    for (auto field : {"parent_device", "parent_inode", "target_device", "target_inode"}) {
        if (!record[field].isUInt64()) Fail("malformed intent identity");
    }
}

bool Bound(const Json::Value& record, const fs::path& target) {
    const auto parent = Stat(target.parent_path());
    const auto st = Stat(target);
    return S_ISDIR(st.st_mode) && record["target"].asString() == target.string() &&
        record["parent_device"].asUInt64() == static_cast<uint64_t>(parent.st_dev) &&
        record["parent_inode"].asUInt64() == static_cast<uint64_t>(parent.st_ino) &&
        record["target_device"].asUInt64() == static_cast<uint64_t>(st.st_dev) &&
        record["target_inode"].asUInt64() == static_cast<uint64_t>(st.st_ino);
}

// An existing but empty/partial/unknown control directory is unresolved, never
// permission to start. Only a well-formed completed record releases the fence.
bool Load(const fs::path& target, Json::Value& record, bool sync_completed = false) {
    const auto control = target.parent_path() / kControl;
    struct stat st{};
    if (::lstat(control.c_str(), &st) != 0) {
        if (errno == ENOENT) return false;
        Fail("control directory unreadable");
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) Fail("invalid control directory");
    for (const auto& entry : fs::directory_iterator(control)) {
        if (entry.path().filename() != "intent.json") Fail("partial or unknown control record");
    }
    Fd dir(::open(control.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (dir.value < 0) Fail("control open failed");
    struct stat opened{};
    if (::fstat(dir.value, &opened) != 0 || opened.st_dev != st.st_dev ||
        opened.st_ino != st.st_ino || opened.st_uid != ::geteuid()) Fail("control identity changed");
    Fd file(::openat(dir.value, "intent.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.value < 0 || ::fstat(file.value, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != ::geteuid() || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > kMaxRecord) Fail("invalid intent file");
    std::string text(static_cast<size_t>(st.st_size), '\0');
    size_t offset = 0;
    while (offset < text.size()) {
        const auto n = ::read(file.value, text.data() + offset, text.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) Fail("intent read failed");
        offset += static_cast<size_t>(n);
    }
    Json::CharReaderBuilder reader;
    reader["rejectDupKeys"] = true;
    reader["failIfExtra"] = true;
    std::istringstream input(text);
    std::string error;
    if (!Json::parseFromStream(reader, input, &record, &error)) Fail("intent parse failed");
    ValidateRecord(record);
    // JsonCpp parses small positive JSON integers as signed values. Normalize
    // identities before comparing with the UInt64 values made by Prepare.
    for (auto field : {"parent_device", "parent_inode", "target_device", "target_inode"}) {
        record[field] = Json::UInt64(record[field].asUInt64());
    }
    const auto parent = Stat(target.parent_path());
    if (record["parent_device"].asUInt64() != static_cast<uint64_t>(parent.st_dev) ||
        record["parent_inode"].asUInt64() != static_cast<uint64_t>(parent.st_ino)) Fail("intent parent identity changed");
    if (sync_completed && record["phase"] == "completed") {
        // A successful rename may be visible even when Store's final fsync
        // returned an error. Re-establish persistence before allowing Start;
        // reading "completed" from the page cache alone is insufficient.
        // The completion validator ran before this record was published. If
        // it survived an unacknowledged write, it is safe to roll it forward
        // once these syncs succeed. Prepared/partial records still block.
        Fd parent_fd(::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        struct stat opened_parent{};
        if (parent_fd.value < 0 || ::fstat(parent_fd.value, &opened_parent) != 0 ||
            opened_parent.st_dev != parent.st_dev || opened_parent.st_ino != parent.st_ino) Fail("parent identity changed");
        if (::fsync(file.value) != 0 || ::fsync(dir.value) != 0 || ::fsync(parent_fd.value) != 0) {
            Fail("completed intent durability unresolved");
        }
        const auto named_control = Stat(control);
        struct stat named_file{};
        if (named_control.st_dev != opened.st_dev || named_control.st_ino != opened.st_ino ||
            ::fstatat(dir.value, "intent.json", &named_file, AT_SYMLINK_NOFOLLOW) != 0 ||
            named_file.st_dev != st.st_dev || named_file.st_ino != st.st_ino ||
            named_file.st_size != st.st_size) Fail("completed intent identity changed");
    }
    return true;
}

void Store(const Json::Value& record) {
    const auto target = Target(record["target"].asString());
    if (!Bound(record, target)) Fail("intent target identity changed");
    const auto control = target.parent_path() / kControl;
    Fd parent(::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent.value < 0) Fail("parent open failed");
    struct stat opened_parent{};
    if (::fstat(parent.value, &opened_parent) != 0 ||
        static_cast<uint64_t>(opened_parent.st_dev) != record["parent_device"].asUInt64() ||
        static_cast<uint64_t>(opened_parent.st_ino) != record["parent_inode"].asUInt64()) Fail("parent identity changed");
    if (::mkdirat(parent.value, kControl, 0700) != 0 && errno != EEXIST) Fail("control creation failed");
    if (::fsync(parent.value) != 0) Fail("parent sync failed");
    const auto st = Stat(control);
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) Fail("invalid control directory");
    Fd dir(::openat(parent.value, kControl, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (dir.value < 0) Fail("control open failed");
    struct stat opened_control{};
    if (::fstat(dir.value, &opened_control) != 0 || opened_control.st_dev != st.st_dev ||
        opened_control.st_ino != st.st_ino || opened_control.st_uid != ::geteuid()) Fail("control identity changed");
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const auto text = Json::writeString(writer, record) + "\n";
    if (text.size() > kMaxRecord) Fail("intent too large");
    Fd file(::openat(dir.value, "intent.next", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.value < 0) Fail("intent staging failed");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto n = ::write(file.value, text.data() + offset, text.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) Fail("intent write failed");
        offset += static_cast<size_t>(n);
    }
    if (::fsync(file.value) != 0) Fail("intent sync failed");
    if (::renameat(dir.value, "intent.next", dir.value, "intent.json") != 0) Fail("intent publish failed");
    if (::fsync(dir.value) != 0) Fail("intent directory sync failed");
}
} // namespace

std::string MaintenanceJournal::RandomId() {
    std::array<unsigned char, 16> data{};
    if (RAND_bytes(data.data(), data.size()) != 1) Fail("random identity failed");
    return Hex(data.data(), data.size());
}

bool MaintenanceJournal::SameTarget(const std::string& a, const std::string& b) {
    try { return fs::equivalent(Target(a), Target(b)); } catch (...) { return false; }
}

int MaintenanceJournal::StartupGate(const std::string& input, Json::Value* info) {
    try {
        Json::Value record;
        if (!Load(Target(input), record, true)) {
            if (info) (*info)["phase"] = "none";
            return NODECORE_OK;
        }
        if (info) {
            (*info)["phase"] = record["phase"];
            (*info)["operation_id"] = record["operation_id"];
        }
        return record["phase"] == "completed" ? NODECORE_OK : NODECORE_ERROR_RECOVERY_REQUIRED;
    } catch (...) {
        if (info) (*info)["phase"] = "unresolved";
        return NODECORE_ERROR_RECOVERY_REQUIRED;
    }
}

int MaintenanceJournal::Prepare(const std::string& input) {
    if (StartupGate(input) != NODECORE_OK) return NODECORE_ERROR_RECOVERY_REQUIRED;
    try {
        const auto target = Target(input);
        const auto parent = Stat(target.parent_path());
        const auto st = Stat(target);
        Json::Value record;
        record["format"] = 1;
        record["plan"] = kInspection;
        record["phase"] = "prepared";
        record["target"] = target.string();
        record["operation_id"] = RandomId();
        record["inventory"] = Inventory(target);
        record["parent_device"] = Json::UInt64(parent.st_dev);
        record["parent_inode"] = Json::UInt64(parent.st_ino);
        record["target_device"] = Json::UInt64(st.st_dev);
        record["target_inode"] = Json::UInt64(st.st_ino);
        Store(record);
        record_ = std::move(record);
        return NODECORE_OK;
    } catch (...) { return NODECORE_ERROR_MAINTENANCE_IO; }
}

int MaintenanceJournal::Resume(const std::string& input, const std::string& operation) {
    try {
        const auto target = Target(input);
        Json::Value record;
        if (!Load(target, record) || record["phase"] != "prepared" ||
            record["operation_id"] != operation || !Bound(record, target)) return NODECORE_ERROR_MAINTENANCE_VALIDATION;
        record_ = std::move(record);
        return NODECORE_OK;
    } catch (...) { return NODECORE_ERROR_RECOVERY_REQUIRED; }
}

int MaintenanceJournal::CompleteUnchanged() {
    try {
        const auto target = Target(record_["target"].asString());
        if (!Bound(record_, target) || Inventory(target) != record_["inventory"].asString()) {
            return NODECORE_ERROR_MAINTENANCE_VALIDATION;
        }
        Json::Value current;
        if (!Load(target, current, true)) return NODECORE_ERROR_MAINTENANCE_VALIDATION;
        if (current["phase"] == "completed") {
            // Retry by the same owner after publication succeeded but its
            // acknowledgement failed. Compare every binding, and revalidate
            // unchanged contents above; never accept another operation's receipt.
            auto expected = record_;
            expected["phase"] = "completed";
            if (current != expected) return NODECORE_ERROR_MAINTENANCE_VALIDATION;
            record_ = std::move(current);
            return NODECORE_OK;
        }
        if (current != record_) return NODECORE_ERROR_MAINTENANCE_VALIDATION;
        auto completed = record_;
        completed["phase"] = "completed";
        Store(completed);
        record_ = std::move(completed);
        return NODECORE_OK;
    } catch (...) { return NODECORE_ERROR_MAINTENANCE_IO; }
}

int MaintenanceJournal::RetainUncertainty() {
    try {
        Json::Value current;
        if (!Load(Target(record_["target"].asString()), current) || current != record_) {
            return NODECORE_ERROR_MAINTENANCE_VALIDATION;
        }
        auto pending = record_;
        pending["phase"] = "prepared";
        Store(pending);
        record_ = std::move(pending);
        return NODECORE_OK;
    } catch (...) { return NODECORE_ERROR_MAINTENANCE_IO; }
}
} // namespace dinero::nodecore
