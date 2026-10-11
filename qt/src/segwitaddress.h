// Copyright (c) 2026 Dinero Labs.
//
// Dinero segwit addresses (bech32 / bech32m) to scriptPubKey.

#pragma once

#include <QString>

namespace segwitaddress {

/// `din1p…` (Taproot, witness v1) or `din1r…` (P2MR, witness v3) on din/tdin/rdin,
/// as scriptPubKey hex. Empty unless the checksum, version and 32-byte program are valid.
QString addressToScriptHex(const QString& address);

}  // namespace segwitaddress
