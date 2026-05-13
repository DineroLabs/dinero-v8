#include "crypto/evp_secp256k1.h"

// The canonical secp256k1 helpers are header-only so all linked targets see the
// same implementation without depending on CMake source wiring for this file.
