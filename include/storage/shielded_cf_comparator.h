#pragma once

// The v1 shielded-state column family deliberately has a persisted comparator
// name distinct from RocksDB's default. Its ordering is exactly bytewise.
// Keep this comparator alive for every DB handle that opens the family.
#include <rocksdb/comparator.h>
#include <rocksdb/options.h>

#include <string>

namespace dinero::storage {

class ShieldedStateBytewiseComparator final : public rocksdb::Comparator {
public:
    const char* Name() const override { return "dinero.shielded_state_v1.bytewise.v1"; }

    int Compare(const rocksdb::Slice& a, const rocksdb::Slice& b) const override {
        return rocksdb::BytewiseComparator()->Compare(a, b);
    }
    bool Equal(const rocksdb::Slice& a, const rocksdb::Slice& b) const override {
        return rocksdb::BytewiseComparator()->Equal(a, b);
    }
    void FindShortestSeparator(std::string* start, const rocksdb::Slice& limit) const override {
        rocksdb::BytewiseComparator()->FindShortestSeparator(start, limit);
    }
    void FindShortSuccessor(std::string* key) const override {
        rocksdb::BytewiseComparator()->FindShortSuccessor(key);
    }
    bool IsSameLengthImmediateSuccessor(const rocksdb::Slice& a,
                                        const rocksdb::Slice& b) const override {
        return rocksdb::BytewiseComparator()->IsSameLengthImmediateSuccessor(a, b);
    }
    bool CanKeysWithDifferentByteContentsBeEqual() const override {
        return rocksdb::BytewiseComparator()->CanKeysWithDifferentByteContentsBeEqual();
    }
};

inline const rocksdb::Comparator* ShieldedStateComparator() {
    static const ShieldedStateBytewiseComparator comparator;
    return &comparator;
}

inline rocksdb::ColumnFamilyOptions ShieldedStateColumnFamilyOptions(
    rocksdb::ColumnFamilyOptions options = {}) {
    options.comparator = ShieldedStateComparator();
    return options;
}

}  // namespace dinero::storage
