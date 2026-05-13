# RocksDB Compression Guide for Dinero

This guide explains how Dinero uses vendored compression libraries (Snappy, LZ4, Zstd) to optimize blockchain storage performance across all platforms.

## Overview

Dinero vendors **three compression libraries** statically to ensure optimal RocksDB performance:

| Library | Version | Speed | Ratio | Use Case |
|---------|---------|-------|-------|----------|
| **Snappy** | v1.1.10 | Fastest | ~1.3-2× | Hot levels, low latency |
| **LZ4** | v1.9.4 | Very Fast | ~1.5-2.5× | Balanced performance |
| **Zstd** | v1.5.6 | Tunable | ~2-4×+ | Cold levels, best ratio |

## Why Compression Matters for Blockchain

### Storage Efficiency
- **Smaller chainstate**: Compressed SST files reduce disk usage by 50-75%
- **Faster sync**: Less data to read/write during initial blockchain download
- **Reduced I/O**: Compression often improves throughput despite CPU overhead

### Performance Benefits
- **LSM-tree optimization**: RocksDB's tiered storage benefits from per-level compression
- **Network efficiency**: Smaller files mean faster replication and backup
- **Memory efficiency**: Compressed blocks use less page cache

## Vendoring Benefits

### ✅ **True Static Builds**
```bash
# No external dependencies needed at runtime
otool -L dinerod | grep -E "(snappy|lz4|zstd)" # Empty on macOS
ldd dinerod | grep -E "(snappy|lz4|zstd)"      # Empty on Linux  
dumpbin /dependents dinerod.exe | findstr /i "snappy lz4 zstd" # Empty on Windows
```

### ✅ **Reproducible Performance**
- **Exact versions**: Snappy v1.1.10, LZ4 v1.9.4, Zstd v1.5.6
- **Identical behavior**: Same compression ratios across all platforms
- **No surprises**: No "RocksDB built without compression" runtime errors

### ✅ **Universal Compatibility**
- **Windows**: MSVC `/MT` static CRT
- **macOS**: Universal arm64/x86_64 support
- **Linux**: No system package dependencies

### ✅ **Security & Supply Chain**
- **Controlled sources**: We control exactly what compression code ships
- **No system dependencies**: Immune to system package manager changes
- **Pinned versions**: No accidental upgrades breaking compatibility

## RocksDB Compression Strategy

### Default Configuration (Recommended)

```cpp
// Example RocksDB configuration for Dinero blockchain storage
rocksdb::Options options;

// Per-level compression strategy
options.compression_per_level = {
    rocksdb::kNoCompression,   // L0: Hot data, frequent writes
    rocksdb::kLZ4Compression,  // L1: Balanced speed/ratio
    rocksdb::kLZ4Compression,  // L2: Still relatively hot
    rocksdb::kZSTD,            // L3: Colder data, better ratio
    rocksdb::kZSTD,            // L4+: Coldest data, maximum compression
};

// Bottom-most level gets maximum compression
options.bottommost_compression = rocksdb::kZSTD;

// Zstd tuning for bottom level
rocksdb::CompressionOptions zstd_opts;
zstd_opts.level = 7;  // Good balance of speed/ratio
zstd_opts.parallel_threads = 2;  // Use multiple threads for compression
options.bottommost_compression_opts = zstd_opts;

// Enable compression for WAL (Write-Ahead Log) if beneficial
options.wal_compression = rocksdb::kLZ4Compression;
```

### Compression Level Rationale

**L0 (No Compression)**:
- Frequent writes from memtable flushes
- Data will be compacted soon anyway
- Minimize write latency

**L1-L2 (LZ4)**:
- Still relatively hot data
- Good balance of speed and compression
- Fast decompression for reads

**L3+ (Zstd)**:
- Colder data, read less frequently
- Maximum space savings
- Acceptable decompression cost

**Bottom-most (Zstd Level 7)**:
- Oldest data, rarely accessed
- Maximum compression ratio
- Long-term storage optimization

## Performance Tuning

### Blockchain-Specific Optimizations

```cpp
// Optimize for blockchain workload
options.level0_file_num_compaction_trigger = 4;
options.level0_slowdown_writes_trigger = 20;
options.level0_stop_writes_trigger = 36;

// Larger block sizes for better compression
options.table_factory.reset(rocksdb::NewBlockBasedTableFactory());
auto table_options = options.table_factory->GetOptions<rocksdb::BlockBasedTableOptions>();
table_options->block_size = 64 * 1024;  // 64KB blocks

// Enable bloom filters to reduce I/O
table_options->filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));

// Optimize for SSD storage
options.compaction_style = rocksdb::kCompactionStyleLevel;
options.max_background_compactions = 4;
options.max_background_flushes = 2;
```

### Memory vs Storage Trade-offs

```cpp
// High-memory configuration (faster, more RAM usage)
options.write_buffer_size = 256 * 1024 * 1024;  // 256MB
options.max_write_buffer_number = 6;
options.target_file_size_base = 256 * 1024 * 1024;  // 256MB

// Low-memory configuration (slower, less RAM usage)  
options.write_buffer_size = 64 * 1024 * 1024;   // 64MB
options.max_write_buffer_number = 3;
options.target_file_size_base = 64 * 1024 * 1024;   // 64MB
```

## Build Configuration

### Enable All Compression (Default)
```bash
cmake .. -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=ON -DDINERO_WITH_ZSTD=ON
```

### Minimal Build (Snappy only)
```bash
cmake .. -DDINERO_WITH_SNAPPY=ON -DDINERO_WITH_LZ4=OFF -DDINERO_WITH_ZSTD=OFF
```

### Maximum Compression (Zstd only)
```bash
cmake .. -DDINERO_WITH_SNAPPY=OFF -DDINERO_WITH_LZ4=OFF -DDINERO_WITH_ZSTD=ON
```

## Monitoring & Metrics

### Key Metrics to Track

```cpp
// RocksDB statistics to monitor
options.statistics = rocksdb::CreateDBStatistics();

// Important metrics:
// - rocksdb.compact.read.bytes
// - rocksdb.compact.write.bytes  
// - rocksdb.compression.times.nanos
// - rocksdb.decompression.times.nanos
// - rocksdb.block.cache.hit
// - rocksdb.block.cache.miss
```

### Compression Effectiveness

```bash
# Check actual compression ratios
du -sh /path/to/blockchain_data/  # Total size
# Compare with uncompressed estimates from RocksDB stats
```

## Platform-Specific Notes

### Windows
- Uses MSVC `/MT` static runtime
- All compression libraries built with same CRT
- No DLL dependencies at runtime

### macOS  
- Universal binaries (arm64 + x86_64)
- No Homebrew dependencies
- Optimized for Apple Silicon performance

### Linux
- Works on any glibc 2.17+ system
- No system package requirements
- Optimized for server workloads

## Future Optimizations

### Adaptive Compression
- Monitor compression ratios in real-time
- Adjust per-level compression based on data patterns
- Use different compression for different data types (blocks vs indices)

### Tiered Storage
- Hot data on fast SSD with light compression
- Cold data on slower storage with maximum compression
- Automatic data migration based on access patterns

### Backup & Archive
- Use maximum Zstd compression for blockchain snapshots
- Parallel compression for faster backup creation
- Incremental backups with delta compression

## Troubleshooting

### Build Issues
```bash
# Verify compression libraries are found
cmake .. -DDINERO_VENDOR_ROCKSDB=ON 2>&1 | grep -E "(Snappy|LZ4|Zstd)"

# Check static linking
otool -L build/bin/dinerod | grep -v /usr/lib | grep -v /System
```

### Runtime Issues
```bash
# Check RocksDB compression support
strings dinerod | grep -E "(snappy|lz4|zstd)"

# Monitor compression performance
iostat -x 1  # Watch I/O patterns during sync
```

### Performance Tuning
```bash
# Profile compression overhead
perf record -g ./dinerod -regtest
perf report | grep -E "(compress|decompress)"
```

## References

- [RocksDB Compression Documentation](https://github.com/facebook/rocksdb/wiki/Compression)
- [Snappy Performance](https://github.com/google/snappy)
- [LZ4 Benchmarks](https://github.com/lz4/lz4)
- [Zstandard Tuning](https://github.com/facebook/zstd)

---

*This guide reflects Dinero's vendored compression setup as of August 2025. Compression ratios and performance characteristics may vary based on blockchain data patterns and hardware configuration.*
