// zk_stubs_windows.cpp
// Stub implementations for Windows MinGW builds without Rust bulletproofs FFI.
//
// This file intentionally only provides the Bulletproofs C FFI surface.
// The native C++ confidential transaction, GPU, and hardware-wallet codepaths
// are built from their normal sources on Windows.

#ifdef _WIN32

#include <cstddef>
#include <cstdint>

extern "C" {

int bp_init(void) { return 0; }
int bp_is_initialized(void) { return 0; }

int bp_generate(uint64_t, const uint8_t*, uint8_t*, size_t*) { return -1; }
int bp_verify(const uint8_t*, const uint8_t*, size_t) { return -1; }
int bp_verify_batch(const uint8_t**, const uint8_t**, const size_t*, size_t) { return -1; }
int bp_generate_with_nonce(uint64_t, const uint8_t*, const uint8_t*, uint8_t*, size_t*) { return -1; }
int bp_rewind(const uint8_t*, const uint8_t*, size_t, const uint8_t*, uint64_t*, uint8_t*) { return -1; }

size_t bp_max_proof_size(size_t) { return 0; }
const char* bp_version(void) { return "stub-windows"; }

int commitment_add(const uint8_t*, const uint8_t*, uint8_t*) { return -1; }
int commitment_sub(const uint8_t*, const uint8_t*, uint8_t*) { return -1; }
int commitment_from_value(uint64_t, uint8_t*) { return -1; }
int commitment_create(uint64_t, const uint8_t*, uint8_t*) { return -1; }
int commitment_is_identity(const uint8_t*) { return -1; }
int generate_random_blinding(uint8_t*) { return -1; }

}  // extern "C"

#endif  // _WIN32
