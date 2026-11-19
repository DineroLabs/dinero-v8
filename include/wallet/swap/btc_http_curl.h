#pragma once
// HTTPS for the public Bitcoin server client (btc_esplora.h), over libcurl
// (docs/design/din-btc-swaps-without-bitcoin-core.md, section 3.1).
//
// The only file of the light Bitcoin client that links libcurl. Each request
//   - goes to an https address, or to plain http on this machine only;
//   - checks the server's certificate and its host name; a failed check is
//     "no answer", never a downgrade;
//   - follows no redirects (a server cannot send us elsewhere);
//   - has a limit on connecting, on the whole exchange, and on the size of the
//     reply (after decompression): a reply over the limit is cut off there;
//   - sends no cookies and nothing that identifies the user.

#include "wallet/swap/btc_esplora.h"

#include <cstddef>
#include <string>

namespace dinero::swap {

struct BtcHttpOptions {
    long connect_timeout_seconds{10};
    long total_timeout_seconds{30};
    size_t max_body_bytes{kEsploraMaxBody};
    // Certificate authorities to trust, as a PEM file. Empty: the system's
    // (macOS, Linux). Windows builds ship a bundle and name it here.
    std::string ca_bundle_path;
};

// A reply larger than the limit comes back with max_body_bytes + 1 bytes, which
// EsploraServer refuses as malformed. Status 0: no answer.
BtcHttp MakeBtcHttp(BtcHttpOptions options = {});

}  // namespace dinero::swap
