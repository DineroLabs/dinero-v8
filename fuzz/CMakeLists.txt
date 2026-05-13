# LibFuzzer targets for Dinero
# Build with: cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++

if(NOT ENABLE_FUZZING)
    return()
endif()

# Check for libFuzzer support
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Fuzzing requires Clang compiler")
    return()
endif()

# Fuzzing flags
set(FUZZ_FLAGS "-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FUZZ_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${FUZZ_FLAGS}")

# HTTP Parser Fuzzer
add_executable(http_parser_fuzz http_parser_fuzz.cpp)
target_include_directories(http_parser_fuzz PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Base64 Fuzzer
add_executable(base64_fuzz base64_fuzz.cpp)
target_include_directories(base64_fuzz PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# JSON-RPC Fuzzer
add_executable(json_rpc_fuzz json_rpc_fuzz.cpp)
target_include_directories(json_rpc_fuzz PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(json_rpc_fuzz PRIVATE jsoncpp_static)

# Create fuzz corpus directories
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/http_parser)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/base64)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/json_rpc)

# Seed corpus files
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/http_parser/valid_request.txt
    "POST / HTTP/1.1\r\nAuthorization: Basic dGVzdDp0ZXN0\r\nContent-Length: 0\r\n\r\n")

file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/base64/valid_base64.txt "dGVzdA==")
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/base64/invalid_base64.txt "dGVzdA!")

file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/json_rpc/valid_request.txt
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockchaininfo\"}")

# Custom targets for running fuzzers
add_custom_target(fuzz_http
    COMMAND $<TARGET_FILE:http_parser_fuzz> 
        ${CMAKE_BINARY_DIR}/fuzz_corpus/http_parser 
        -max_total_time=60
        -print_final_stats=1
    DEPENDS http_parser_fuzz
    COMMENT "Running HTTP parser fuzzer for 60 seconds"
)

add_custom_target(fuzz_base64
    COMMAND $<TARGET_FILE:base64_fuzz> 
        ${CMAKE_BINARY_DIR}/fuzz_corpus/base64 
        -max_total_time=60
        -print_final_stats=1
    DEPENDS base64_fuzz
    COMMENT "Running Base64 fuzzer for 60 seconds"
)

add_custom_target(fuzz_json_rpc
    COMMAND $<TARGET_FILE:json_rpc_fuzz> 
        ${CMAKE_BINARY_DIR}/fuzz_corpus/json_rpc 
        -max_total_time=60
        -print_final_stats=1
    DEPENDS json_rpc_fuzz
    COMMENT "Running JSON-RPC fuzzer for 60 seconds"
)

# ============================================================================
# Phase 27: Script Interpreter Fuzzer (Critical for consensus security)
# ============================================================================

# Script Interpreter Fuzzer - Tests EvalScript, VerifyScript, CHECKSIG
add_executable(fuzz_script fuzz_script.cpp)
target_include_directories(fuzz_script PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_script PRIVATE
    dinero_consensus
    secp256k1
    OpenSSL::Crypto
)

# Corpus generator (standalone, no fuzzing flags)
add_executable(generate_corpus generate_corpus.cpp)
set_target_properties(generate_corpus PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)

# DER Signature Parser Fuzzer
add_executable(fuzz_der fuzz_der.cpp)
target_include_directories(fuzz_der PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_der PRIVATE
    dinero_consensus
    secp256k1
    OpenSSL::Crypto
)

# Sighash Computation Fuzzer
add_executable(fuzz_sighash fuzz_sighash.cpp)
target_include_directories(fuzz_sighash PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_sighash PRIVATE
    dinero_consensus
    secp256k1
    OpenSSL::Crypto
)

# Corpus directories for script fuzzer
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/script)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/script)

# Custom target to generate initial corpus
add_custom_target(generate_script_corpus
    COMMAND $<TARGET_FILE:generate_corpus> ${CMAKE_BINARY_DIR}/fuzz_corpus/script
    DEPENDS generate_corpus
    COMMENT "Generating initial script fuzzer corpus"
)

# Run script fuzzer
add_custom_target(fuzz_script_run
    COMMAND $<TARGET_FILE:fuzz_script>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/script
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/script/
        -max_len=10000
        -max_total_time=300
        -print_final_stats=1
    DEPENDS fuzz_script generate_script_corpus
    COMMENT "Running Script interpreter fuzzer for 5 minutes"
)

# Quick script fuzz (60 seconds)
add_custom_target(fuzz_script_quick
    COMMAND $<TARGET_FILE:fuzz_script>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/script
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/script/
        -max_len=10000
        -max_total_time=60
        -print_final_stats=1
    DEPENDS fuzz_script
    COMMENT "Running Script interpreter fuzzer for 60 seconds"
)

# Run all fuzzers
add_custom_target(fuzz_all
    DEPENDS fuzz_http fuzz_base64 fuzz_json_rpc fuzz_script_run
    COMMENT "Running all fuzzers"
)

# Corpus directories for DER and sighash fuzzers
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/der)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/sighash)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/der)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/sighash)

# Run DER signature fuzzer
add_custom_target(fuzz_der_run
    COMMAND $<TARGET_FILE:fuzz_der>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/der
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/der/
        -max_len=200
        -max_total_time=120
        -print_final_stats=1
    DEPENDS fuzz_der
    COMMENT "Running DER signature fuzzer for 2 minutes"
)

# Run sighash fuzzer
add_custom_target(fuzz_sighash_run
    COMMAND $<TARGET_FILE:fuzz_sighash>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/sighash
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/sighash/
        -max_len=5000
        -max_total_time=120
        -print_final_stats=1
    DEPENDS fuzz_sighash
    COMMENT "Running sighash computation fuzzer for 2 minutes"
)

# Run consensus-critical fuzzers only (script, DER, sighash)
add_custom_target(fuzz_consensus
    DEPENDS fuzz_script_run fuzz_der_run fuzz_sighash_run
    COMMENT "Running all consensus-critical fuzzers"
)

# Quick consensus fuzz (60 seconds each)
add_custom_target(fuzz_consensus_quick
    COMMAND $<TARGET_FILE:fuzz_script>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/script -max_len=10000 -max_total_time=60
    COMMAND $<TARGET_FILE:fuzz_der>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/der -max_len=200 -max_total_time=60
    COMMAND $<TARGET_FILE:fuzz_sighash>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/sighash -max_len=5000 -max_total_time=60
    DEPENDS fuzz_script fuzz_der fuzz_sighash
    COMMENT "Running quick consensus fuzzing (60s each target)"
)

# ============================================================================
# Phase 36: P2P Deserialization Fuzzers (Network security)
# ============================================================================

# P2P Header Parser Fuzzer
add_executable(fuzz_p2p_header fuzz_p2p_header.cpp)
target_include_directories(fuzz_p2p_header PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_p2p_header PRIVATE dinero_core OpenSSL::Crypto)

# VARINT Parser Fuzzer (standalone, minimal deps)
add_executable(fuzz_varint fuzz_varint.cpp)
target_include_directories(fuzz_varint PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Transaction Deserialization Fuzzer
add_executable(fuzz_tx_deserialize fuzz_tx_deserialize.cpp)
target_include_directories(fuzz_tx_deserialize PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_tx_deserialize PRIVATE dinero_wallet OpenSSL::Crypto)

# Structural Validator Fuzzer
add_executable(fuzz_structural_validation
    fuzz_structural_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/p2p/structural_validator.cpp
)
target_include_directories(fuzz_structural_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_structural_validation PRIVATE dinero_core OpenSSL::Crypto)

# Corpus directories for P2P fuzzers
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/p2p_header)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/varint)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/tx)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/structural)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/p2p_header)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/varint)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/tx)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/structural)

# Seed corpus will be generated at build time by fuzz targets
# Note: Binary files with specific bytes are created via custom commands below

# Create simple text-based seed files for initial corpus
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/p2p_header/seed1.txt "version")
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/varint/seed1.txt "A")
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/tx/seed1.txt "tx")
file(WRITE ${CMAKE_BINARY_DIR}/fuzz_corpus/structural/seed1.txt "block")

# Run P2P header fuzzer
add_custom_target(fuzz_p2p_header_run
    COMMAND $<TARGET_FILE:fuzz_p2p_header>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/p2p_header
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/p2p_header/
        -max_len=1024
        -max_total_time=120
        -print_final_stats=1
    DEPENDS fuzz_p2p_header
    COMMENT "Running P2P header fuzzer for 2 minutes"
)

# Run VARINT fuzzer
add_custom_target(fuzz_varint_run
    COMMAND $<TARGET_FILE:fuzz_varint>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/varint
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/varint/
        -max_len=64
        -max_total_time=60
        -print_final_stats=1
    DEPENDS fuzz_varint
    COMMENT "Running VARINT fuzzer for 60 seconds"
)

# Run transaction deserialize fuzzer
add_custom_target(fuzz_tx_run
    COMMAND $<TARGET_FILE:fuzz_tx_deserialize>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/tx
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/tx/
        -max_len=100000
        -max_total_time=180
        -print_final_stats=1
    DEPENDS fuzz_tx_deserialize
    COMMENT "Running transaction deserialize fuzzer for 3 minutes"
)

# Run structural validator fuzzer
add_custom_target(fuzz_structural_run
    COMMAND $<TARGET_FILE:fuzz_structural_validation>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/structural
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/structural/
        -max_len=4194304
        -max_total_time=180
        -print_final_stats=1
    DEPENDS fuzz_structural_validation
    COMMENT "Running structural validator fuzzer for 3 minutes"
)

# Run all P2P fuzzers
add_custom_target(fuzz_p2p
    DEPENDS fuzz_p2p_header_run fuzz_varint_run fuzz_tx_run fuzz_structural_run
    COMMENT "Running all P2P deserialization fuzzers"
)

# Quick P2P fuzz (30 seconds each)
add_custom_target(fuzz_p2p_quick
    COMMAND $<TARGET_FILE:fuzz_p2p_header>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/p2p_header -max_len=1024 -max_total_time=30
    COMMAND $<TARGET_FILE:fuzz_varint>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/varint -max_len=64 -max_total_time=30
    COMMAND $<TARGET_FILE:fuzz_tx_deserialize>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/tx -max_len=100000 -max_total_time=30
    COMMAND $<TARGET_FILE:fuzz_structural_validation>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/structural -max_len=4194304 -max_total_time=30
    DEPENDS fuzz_p2p_header fuzz_varint fuzz_tx_deserialize fuzz_structural_validation
    COMMENT "Running quick P2P fuzzing (30s each target)"
)

# Update fuzz_all to include P2P fuzzers
add_custom_target(fuzz_all_complete
    DEPENDS fuzz_http fuzz_base64 fuzz_json_rpc fuzz_script_run fuzz_p2p
    COMMENT "Running ALL fuzzers (including P2P)"
)

# ============================================================================
# Phase D.3: Consensus Safety Fuzzers (CRITICAL - NEVER REMOVE)
# ============================================================================
# These fuzzers provide SAFETY armor for consensus-critical code.
# They test that consensus validation NEVER CRASHES, regardless of input.
#
# Philosophy: D.2 tests CORRECTNESS, D.3 tests SAFETY
# - Never assert result == X
# - Only assert: no crash, no OOB, no UB, no infinite loop
#
# 🔒 WARNING: Once merged, these fuzzers must NEVER be removed.

# Phase D.3.1: Premine Validation Fuzzer
add_executable(fuzz_premine fuzz_premine.cpp)
target_include_directories(fuzz_premine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_premine PRIVATE
    dinero_chainstate  # For ChainDB symbols referenced by premine.cpp
    dinero_consensus
    dinero_core        # For logger symbols used by consensus objects
    OpenSSL::Crypto    # For SHA*/RIPEMD160 symbols used by script_crypto
)

# Phase D.3.2: Consensus Limits Fuzzer
add_executable(fuzz_consensus_limits fuzz_consensus_limits.cpp)
target_include_directories(fuzz_consensus_limits PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
# Header-only, no linking needed

# Phase D.3.3: Transaction Structural Fuzzer (already exists from Phase 36)
# fuzz_tx_deserialize already defined above

# Phase D.3.4: Script Limits Fuzzer (NOT script VM)
add_executable(fuzz_script_limits fuzz_script_limits.cpp)
target_include_directories(fuzz_script_limits PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
# Header-only limits, no linking needed

# Corpus directories for Phase D.3 fuzzers
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/premine)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/consensus_limits)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/script_limits)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/premine)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/consensus_limits)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/script_limits)

# Run Phase D.3.1: Premine fuzzer
add_custom_target(fuzz_premine_run
    COMMAND $<TARGET_FILE:fuzz_premine>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/premine
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/premine/
        -max_len=1024
        -max_total_time=3600
        -print_final_stats=1
    DEPENDS fuzz_premine
    COMMENT "Phase D.3.1: Running premine validation fuzzer for 1 hour"
)

# Run Phase D.3.2: Consensus limits fuzzer
add_custom_target(fuzz_consensus_limits_run
    COMMAND $<TARGET_FILE:fuzz_consensus_limits>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/consensus_limits
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/consensus_limits/
        -max_len=64
        -max_total_time=3600
        -print_final_stats=1
    DEPENDS fuzz_consensus_limits
    COMMENT "Phase D.3.2: Running consensus limits fuzzer for 1 hour"
)

# Run Phase D.3.4: Script limits fuzzer
add_custom_target(fuzz_script_limits_run
    COMMAND $<TARGET_FILE:fuzz_script_limits>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/script_limits
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/script_limits/
        -max_len=20000
        -max_total_time=3600
        -print_final_stats=1
    DEPENDS fuzz_script_limits
    COMMENT "Phase D.3.4: Running script limits fuzzer for 1 hour"
)

# Run ALL Phase D.3 consensus safety fuzzers (1 hour each = 4 hours total)
add_custom_target(fuzz_consensus_safety
    DEPENDS fuzz_premine_run fuzz_consensus_limits_run fuzz_tx_run fuzz_script_limits_run
    COMMENT "Phase D.3: Running ALL consensus safety fuzzers (4 hours total)"
)

# Quick Phase D.3 run (5 minutes each = 20 minutes total)
add_custom_target(fuzz_consensus_safety_quick
    COMMAND $<TARGET_FILE:fuzz_premine>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/premine -max_len=1024 -max_total_time=300
    COMMAND $<TARGET_FILE:fuzz_consensus_limits>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/consensus_limits -max_len=64 -max_total_time=300
    COMMAND $<TARGET_FILE:fuzz_tx_deserialize>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/tx -max_len=100000 -max_total_time=300
    COMMAND $<TARGET_FILE:fuzz_script_limits>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/script_limits -max_len=20000 -max_total_time=300
    DEPENDS fuzz_premine fuzz_consensus_limits fuzz_tx_deserialize fuzz_script_limits
    COMMENT "Phase D.3 Quick: Running consensus safety fuzzers (5min each)"
)

# ============================================================================
# Bulletproofs / ZK Proof Fuzzer (CRITICAL - Inflation Attack Surface)
# ============================================================================
# Adversarial fuzzing of range proof verification.
# A bug here could allow:
# - Inflation (values outside valid range)
# - Denial of service (crash verifier)
# - Memory corruption
#
# Attack surfaces:
# 1. Proof deserialization (malformed bytes)
# 2. Proof verification (corrupted proofs)
# 3. Commitment parsing (invalid curve points)
# 4. Batch verification (mixed valid/invalid)
# 5. Rewind operations (wrong nonces)

# Bulletproofs Range Proof Fuzzer
add_executable(fuzz_bulletproofs fuzz_bulletproofs.cpp)
target_include_directories(fuzz_bulletproofs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_bulletproofs PRIVATE
    dinero_zk
    bulletproofs_ffi
)

# Standalone version (without libFuzzer, for quick testing)
add_executable(fuzz_bulletproofs_standalone fuzz_bulletproofs.cpp)
target_include_directories(fuzz_bulletproofs_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(fuzz_bulletproofs_standalone PRIVATE FUZZ_STANDALONE)
set_target_properties(fuzz_bulletproofs_standalone PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)
target_link_libraries(fuzz_bulletproofs_standalone PRIVATE
    dinero_zk
    bulletproofs_ffi
)

# Corpus directories for bulletproofs fuzzer
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/bulletproofs)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/bulletproofs)

# Run Bulletproofs fuzzer (1 hour)
add_custom_target(fuzz_bulletproofs_run
    COMMAND $<TARGET_FILE:fuzz_bulletproofs>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/bulletproofs
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/bulletproofs/
        -max_len=4096
        -max_total_time=3600
        -print_final_stats=1
    DEPENDS fuzz_bulletproofs
    COMMENT "Running Bulletproofs range proof fuzzer for 1 hour"
)

# Quick Bulletproofs fuzz (5 minutes)
add_custom_target(fuzz_bulletproofs_quick
    COMMAND $<TARGET_FILE:fuzz_bulletproofs>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/bulletproofs
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/bulletproofs/
        -max_len=4096
        -max_total_time=300
        -print_final_stats=1
    DEPENDS fuzz_bulletproofs
    COMMENT "Running Bulletproofs fuzzer for 5 minutes"
)

# Run standalone fuzzer (no libFuzzer needed)
add_custom_target(fuzz_bulletproofs_standalone_run
    COMMAND $<TARGET_FILE:fuzz_bulletproofs_standalone>
    DEPENDS fuzz_bulletproofs_standalone
    COMMENT "Running Bulletproofs standalone fuzzer (10000 iterations)"
)

# Add to consensus safety fuzzers
add_custom_target(fuzz_zk_safety
    DEPENDS fuzz_bulletproofs_run
    COMMENT "Running ZK/Bulletproofs safety fuzzers"
)

# ============================================================================
# Phase Plan-A: Compact Block Binary Wire Format Fuzzer
# ============================================================================
# Fuzzes BIP152 compact block deserialization to ensure:
# - Never crashes on any input
# - Round-trip correctness (parse → serialize → parse)
# - Deterministic serialization
#
# Attack surfaces:
# - CompactBlock: header + nonce + short txids + prefilled txs
# - BlockTransactionsRequest: hash + indexes
# - BlockTransactions: hash + transactions
# - 48-bit short txid handling
# - CompactSize parsing edge cases

# Compact Block Fuzzer (libFuzzer)
add_executable(fuzz_compact_block fuzz_compact_block.cpp)
target_include_directories(fuzz_compact_block PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fuzz_compact_block PRIVATE dinero_core)

# Standalone version (without libFuzzer, for quick testing)
add_executable(fuzz_compact_block_standalone fuzz_compact_block.cpp)
target_include_directories(fuzz_compact_block_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(fuzz_compact_block_standalone PRIVATE FUZZ_STANDALONE)
set_target_properties(fuzz_compact_block_standalone PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)
target_link_libraries(fuzz_compact_block_standalone PRIVATE dinero_core)

# Corpus directories for compact block fuzzer
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/compact_block)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/compact_block)

# Run compact block fuzzer (5 minutes - fast due to simple binary format)
add_custom_target(fuzz_compact_block_run
    COMMAND $<TARGET_FILE:fuzz_compact_block>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/compact_block
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/compact_block/
        -max_len=100000
        -max_total_time=300
        -print_final_stats=1
    DEPENDS fuzz_compact_block
    COMMENT "Phase Plan-A: Running compact block fuzzer for 5 minutes"
)

# Quick compact block fuzz (60 seconds)
add_custom_target(fuzz_compact_block_quick
    COMMAND $<TARGET_FILE:fuzz_compact_block>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/compact_block
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/compact_block/
        -max_len=100000
        -max_total_time=60
        -print_final_stats=1
    DEPENDS fuzz_compact_block
    COMMENT "Phase Plan-A: Running compact block fuzzer for 60 seconds"
)

# Run standalone fuzzer (no libFuzzer needed)
add_custom_target(fuzz_compact_block_standalone_run
    COMMAND $<TARGET_FILE:fuzz_compact_block_standalone>
    DEPENDS fuzz_compact_block_standalone
    COMMENT "Phase Plan-A: Running compact block standalone fuzzer (10000 iterations)"
)

# Add to P2P fuzzers group
add_custom_target(fuzz_p2p_all
    DEPENDS fuzz_p2p fuzz_compact_block_run
    COMMENT "Running all P2P fuzzers including compact blocks"
)

# ============================================================================
# Ring Signature Fuzzers (CRITICAL - Privacy/Consensus Attack Surface)
# ============================================================================
# These fuzzers exercise the CLSAG ring signature infrastructure to catch
# crashes, assertion failures, and edge cases with random/malformed inputs
# before mainnet activation.
#
# Attack surfaces:
# 1. CLSAG signature deserialization (malformed bytes, bad ring size)
# 2. parse_point (all 256 prefix bytes, invalid x-coordinates)
# 3. Ring transaction parsing (v3 wire format with ring fields)
# 4. Full sign/verify pipeline with crafted inputs

# --- CLSAG Signature Deserialization Fuzzer ---
add_executable(fuzz_clsag_deserialize fuzz_clsag_deserialize.cpp)
target_include_directories(fuzz_clsag_deserialize PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_link_libraries(fuzz_clsag_deserialize PRIVATE
    dinero_zk
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_clsag_deserialize)

# Standalone version (no libFuzzer)
add_executable(fuzz_clsag_deserialize_standalone fuzz_clsag_deserialize.cpp)
target_include_directories(fuzz_clsag_deserialize_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_compile_definitions(fuzz_clsag_deserialize_standalone PRIVATE FUZZ_STANDALONE)
set_target_properties(fuzz_clsag_deserialize_standalone PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)
target_link_libraries(fuzz_clsag_deserialize_standalone PRIVATE
    dinero_zk
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_clsag_deserialize_standalone)

# --- parse_point Fuzzer ---
add_executable(fuzz_parse_point fuzz_parse_point.cpp)
target_include_directories(fuzz_parse_point PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_link_libraries(fuzz_parse_point PRIVATE
    dinero_zk
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_parse_point)

# Standalone version
add_executable(fuzz_parse_point_standalone fuzz_parse_point.cpp)
target_include_directories(fuzz_parse_point_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_compile_definitions(fuzz_parse_point_standalone PRIVATE FUZZ_STANDALONE)
set_target_properties(fuzz_parse_point_standalone PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)
target_link_libraries(fuzz_parse_point_standalone PRIVATE
    dinero_zk
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_parse_point_standalone)

# --- Ring Signature Validator Fuzzer ---
add_executable(fuzz_ring_validator fuzz_ring_validator.cpp)
target_include_directories(fuzz_ring_validator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_link_libraries(fuzz_ring_validator PRIVATE
    dinero_zk
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_ring_validator)

# Standalone version
add_executable(fuzz_ring_validator_standalone fuzz_ring_validator.cpp)
target_include_directories(fuzz_ring_validator_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
)
target_compile_definitions(fuzz_ring_validator_standalone PRIVATE FUZZ_STANDALONE)
set_target_properties(fuzz_ring_validator_standalone PROPERTIES
    COMPILE_FLAGS "-O2 -std=c++17"
    LINK_FLAGS ""
)
target_link_libraries(fuzz_ring_validator_standalone PRIVATE
    dinero_zk
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
)
link_secp256k1_zkp(fuzz_ring_validator_standalone)

# --- Corpus directories ---
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/clsag_deser)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/parse_point)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_corpus/ring_validator)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/clsag_deser)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/parse_point)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz_crashes/ring_validator)

# --- Run targets ---

# CLSAG Deserialize fuzzer (5 minutes)
add_custom_target(fuzz_clsag_deser_run
    COMMAND $<TARGET_FILE:fuzz_clsag_deserialize>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/clsag_deser
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/clsag_deser/
        -max_len=4096
        -max_total_time=300
        -print_final_stats=1
    DEPENDS fuzz_clsag_deserialize
    COMMENT "Running CLSAG deserialization fuzzer for 5 minutes"
)

# parse_point fuzzer (5 minutes)
add_custom_target(fuzz_parse_point_run
    COMMAND $<TARGET_FILE:fuzz_parse_point>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/parse_point
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/parse_point/
        -max_len=256
        -max_total_time=300
        -print_final_stats=1
    DEPENDS fuzz_parse_point
    COMMENT "Running parse_point fuzzer for 5 minutes"
)

# Ring validator fuzzer (10 minutes - crypto-heavy)
add_custom_target(fuzz_ring_validator_run
    COMMAND $<TARGET_FILE:fuzz_ring_validator>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/ring_validator
        -artifact_prefix=${CMAKE_BINARY_DIR}/fuzz_crashes/ring_validator/
        -max_len=8192
        -max_total_time=600
        -print_final_stats=1
    DEPENDS fuzz_ring_validator
    COMMENT "Running ring signature validator fuzzer for 10 minutes"
)

# Run standalone fuzzers (no libFuzzer needed)
add_custom_target(fuzz_clsag_deser_standalone_run
    COMMAND $<TARGET_FILE:fuzz_clsag_deserialize_standalone>
    DEPENDS fuzz_clsag_deserialize_standalone
    COMMENT "Running CLSAG deserialize standalone fuzzer (10000 iterations)"
)
add_custom_target(fuzz_parse_point_standalone_run
    COMMAND $<TARGET_FILE:fuzz_parse_point_standalone>
    DEPENDS fuzz_parse_point_standalone
    COMMENT "Running parse_point standalone fuzzer (10000 iterations)"
)
add_custom_target(fuzz_ring_validator_standalone_run
    COMMAND $<TARGET_FILE:fuzz_ring_validator_standalone>
    DEPENDS fuzz_ring_validator_standalone
    COMMENT "Running ring validator standalone fuzzer (1000 iterations)"
)

# Run ALL ring signature fuzzers
add_custom_target(fuzz_ring_all
    DEPENDS fuzz_clsag_deser_run fuzz_parse_point_run fuzz_ring_validator_run
    COMMENT "Running all ring signature fuzzers"
)

# Quick ring fuzz (60 seconds each)
add_custom_target(fuzz_ring_quick
    COMMAND $<TARGET_FILE:fuzz_clsag_deserialize>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/clsag_deser -max_len=4096 -max_total_time=60
    COMMAND $<TARGET_FILE:fuzz_parse_point>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/parse_point -max_len=256 -max_total_time=60
    COMMAND $<TARGET_FILE:fuzz_ring_validator>
        ${CMAKE_BINARY_DIR}/fuzz_corpus/ring_validator -max_len=8192 -max_total_time=60
    DEPENDS fuzz_clsag_deserialize fuzz_parse_point fuzz_ring_validator
    COMMENT "Running quick ring signature fuzzing (60s each target)"
)

# Run ALL standalone ring fuzzers (no libFuzzer needed)
add_custom_target(fuzz_ring_standalone_all
    DEPENDS fuzz_clsag_deser_standalone_run fuzz_parse_point_standalone_run
            fuzz_ring_validator_standalone_run
    COMMENT "Running all ring signature standalone fuzzers"
)
