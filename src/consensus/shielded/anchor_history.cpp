#include "consensus/shielded/anchor_history.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dinero::consensus::shielded {

namespace {
constexpr uint32_t kFileMagic   = 0xA0C30001;
constexpr uint16_t kFileVersion = 1;

template <typename T>
void WriteLE(std::ostream& os, T v) {
    static_assert(std::is_unsigned_v<T>, "WriteLE requires unsigned integer");
    uint8_t buf[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) {
        buf[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    os.write(reinterpret_cast<const char*>(buf), sizeof(T));
}

template <typename T>
bool ReadLE(std::istream& is, T& out) {
    static_assert(std::is_unsigned_v<T>, "ReadLE requires unsigned integer");
    uint8_t buf[sizeof(T)];
    is.read(reinterpret_cast<char*>(buf), sizeof(T));
    if (is.gcount() != static_cast<std::streamsize>(sizeof(T))) return false;
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        v |= static_cast<T>(buf[i]) << (8 * i);
    }
    out = v;
    return true;
}
}  // namespace

void AnchorHistory::RecordRoot(uint32_t height, const Hash& root) {
    // Idempotent overwrite: if the most recent entry is at the same
    // height, replace it instead of stacking duplicates. (Happens on
    // re-validation passes that re-apply a block already at the tip.)
    if (!roots_.empty() && roots_.back().first == height) {
        roots_.back().second = root;
        return;
    }
    roots_.emplace_back(height, root);
    while (roots_.size() > kDepth) {
        // Retain what we evict so the matching disconnect can put it back.
        // Eviction used to be lossy while RollbackAbove only deleted, so a
        // disconnect left this node with a strictly smaller window than a
        // never-reorged peer at the same tip (audit finding #4).
        evicted_.push_back(roots_.front());
        roots_.pop_front();
        while (evicted_.size() > kEvictionRetention) {
            evicted_.pop_front();  // bounded: only the newest evictions matter
        }
    }
}

bool AnchorHistory::Contains(const Hash& candidate) const {
    return std::any_of(roots_.begin(), roots_.end(),
                       [&candidate](const auto& entry) {
                           return entry.second == candidate;
                       });
}

void AnchorHistory::RollbackAbove(uint32_t height) {
    while (!roots_.empty() && roots_.back().first > height) {
        roots_.pop_back();
    }
    // Put back what the disconnected blocks displaced when they connected, so
    // this node ends up with the same window a never-reorged peer at this tip
    // holds. Never grows past kDepth, and only accepts entries strictly older
    // than the current front so the deque stays ascending by height.
    while (roots_.size() < kDepth && !evicted_.empty()) {
        const auto& candidate = evicted_.back();
        if (!roots_.empty() && candidate.first >= roots_.front().first) {
            // Already represented (or out of order) — drop it rather than
            // corrupt the ordering RollbackAbove itself depends on.
            evicted_.pop_back();
            continue;
        }
        roots_.push_front(candidate);
        evicted_.pop_back();
    }
}

std::vector<uint8_t> AnchorHistory::SerializeBytes() const {
    // Mirrors Save()'s on-disk layout exactly so a content hash here
    // is comparable to the saved file. Layout:
    //   [4 LE] kFileMagic | [2 LE] kFileVersion | [2 LE] count
    //   for each entry: [4 LE] height | [32 BE] root
    std::vector<uint8_t> out;
    out.reserve(8 + roots_.size() * (4 + 32));
    auto write_u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto write_u16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    write_u32(kFileMagic);
    write_u16(kFileVersion);
    write_u16(static_cast<uint16_t>(roots_.size()));
    for (const auto& [height, root] : roots_) {
        write_u32(height);
        out.insert(out.end(), root.begin(), root.end());
    }
    return out;
}

AnchorHistory::IoResult AnchorHistory::DeserializeBytes(const std::vector<uint8_t>& bytes) {
    // In-memory mirror of Load(). Parses the same magic / version /
    // count / (height,root)* layout SerializeBytes produces.
    if (bytes.size() < 4 + 2 + 2) {
        roots_.clear();
        return IoResult::Truncated;
    }

    auto read_u16 = [&](size_t off) -> uint16_t {
        return static_cast<uint16_t>(bytes[off]) |
               (static_cast<uint16_t>(bytes[off + 1]) << 8);
    };
    auto read_u32 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(bytes[off]) |
               (static_cast<uint32_t>(bytes[off + 1]) << 8) |
               (static_cast<uint32_t>(bytes[off + 2]) << 16) |
               (static_cast<uint32_t>(bytes[off + 3]) << 24);
    };

    size_t off = 0;
    const uint32_t magic = read_u32(off);
    off += 4;
    if (magic != kFileMagic) {
        roots_.clear();
        return IoResult::FormatError;
    }

    const uint16_t version = read_u16(off);
    off += 2;
    if (version != kFileVersion) {
        roots_.clear();
        return IoResult::VersionMismatch;
    }

    const uint16_t count = read_u16(off);
    off += 2;
    if (count > kDepth) {
        roots_.clear();
        return IoResult::FormatError;
    }

    decltype(roots_) staged;
    constexpr size_t kEntrySize = 4 /*height*/ + 32 /*root*/;
    if (bytes.size() < off + static_cast<size_t>(count) * kEntrySize) {
        roots_.clear();
        return IoResult::Truncated;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t height = read_u32(off);
        off += 4;
        Hash root{};
        std::memcpy(root.data(), bytes.data() + off, root.size());
        off += root.size();
        // Reject same-height duplicates / non-monotonic input — same
        // strict ordering rule Load() enforces.
        if (!staged.empty() && staged.back().first > height) {
            roots_.clear();
            return IoResult::FormatError;
        }
        staged.emplace_back(height, root);
    }
    // Reject extra trailing bytes — strict format.
    if (off != bytes.size()) {
        roots_.clear();
        return IoResult::FormatError;
    }

    roots_ = std::move(staged);
    return IoResult::Ok;
}

AnchorHistory::IoResult AnchorHistory::Save(const std::string& path) const {
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return IoResult::IoError;
        WriteLE<uint32_t>(out, kFileMagic);
        WriteLE<uint16_t>(out, kFileVersion);
        WriteLE<uint16_t>(out, static_cast<uint16_t>(roots_.size()));
        for (const auto& [height, root] : roots_) {
            WriteLE<uint32_t>(out, height);
            out.write(reinterpret_cast<const char*>(root.data()),
                      static_cast<std::streamsize>(root.size()));
        }
        if (!out) return IoResult::IoError;
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return IoResult::IoError;
    }
    return IoResult::Ok;
}

AnchorHistory::IoResult AnchorHistory::Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return IoResult::IoError;

    uint32_t magic = 0;
    if (!ReadLE(in, magic) || magic != kFileMagic) return IoResult::FormatError;

    uint16_t version = 0;
    if (!ReadLE(in, version)) return IoResult::Truncated;
    if (version != kFileVersion) return IoResult::VersionMismatch;

    uint16_t count = 0;
    if (!ReadLE(in, count)) return IoResult::Truncated;
    if (count > kDepth) return IoResult::FormatError;

    decltype(roots_) staged;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t height = 0;
        if (!ReadLE(in, height)) return IoResult::Truncated;
        Hash root{};
        in.read(reinterpret_cast<char*>(root.data()),
                static_cast<std::streamsize>(root.size()));
        if (in.gcount() != static_cast<std::streamsize>(root.size())) {
            return IoResult::Truncated;
        }
        // Reject same-height duplicates / non-monotonic input — the
        // wire format is defined as in insertion order, oldest first.
        if (!staged.empty() && staged.back().first > height) {
            return IoResult::FormatError;
        }
        staged.emplace_back(height, root);
    }
    // Reject extra trailing bytes — strict format.
    in.peek();
    if (!in.eof()) return IoResult::FormatError;

    roots_ = std::move(staged);
    return IoResult::Ok;
}

}  // namespace dinero::consensus::shielded
