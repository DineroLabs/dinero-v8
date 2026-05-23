// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <string>

namespace dinero::network {

// Generates an ephemeral self-signed ECDSA P-256 cert + private key for use
// as the TLS material on QUIC relay sessions. The keypair lives only in the
// daemon's address space — not persisted to disk — and is regenerated on
// every restart. Identity is not asserted by the cert; the relay encryption
// layer is opportunistic and `verify_peer` is always false on the consuming
// QuicSessionOptions. The cert is only here because ngtcp2/quictls require
// some cert/key material to drive the TLS handshake at all.
//
// Returns true on success and writes PEM-encoded strings into the out
// parameters. On failure returns false and writes a human-readable reason
// into *err (if non-null).
bool GenerateRelayTlsKeypair(std::string* cert_pem,
                             std::string* private_key_pem,
                             std::string* err);

}  // namespace dinero::network
