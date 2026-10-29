# Checkpoint interval test owns its RocksDB build dependency

The PR full and QUIC default builds failed because test_chainstate_commit_batch_interval linked the vendored RocksDB archive before its external install completed. Both logs show the same missing archive followed immediately by install. The merge checkout tree exactly matched the PR source; earlier successful scheduling did not establish the missing dependency.

The target now links the existing RocksDB::rocksdb imported target, which owns the vendor installation dependency and configured platform libraries. The test fixture, assertions, production sources and existing test commands are unchanged. The Orchard component workflow builds this existing target and requires all five existing checkpoint interval cases to execute.

Fresh qualification requests the test target from an empty Ninja build without separately prebuilding RocksDB, then builds the full daemon, in ON/OFF configurations. Actual dependency graph and executions are retained privately. Linux full/exact/heavy qualification is required; this CMake repair is not a consensus, callback, sanitizer, platform-load or release-readiness claim. Mainnet activation remains unset.
