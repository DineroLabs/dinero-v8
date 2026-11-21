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
//   - sends no cookies and nothing that identifies the user;
//   - is given up at once when the node stops (InterruptBtcHttp).

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
// Connections are kept open between the requests of one BtcHttp (and of its
// copies), so a run of requests to one server pays for one handshake. A
// server that let a request hang for three seconds or more is not asked
// again for half a minute (GET only; a POST is always tried).
BtcHttp MakeBtcHttp(BtcHttpOptions options = {});

// The node is stopping: every request in flight, in any thread, ends now as
// "no answer", and so does every later one. A stop must not wait out a
// server that has gone quiet. `false` lifts it again (tests).
void InterruptBtcHttp(bool interrupted = true);

}  // namespace dinero::swap
