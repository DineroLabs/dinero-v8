#include <json/json.h>
#include <string>

namespace dinero {
namespace rpc {

// Minimal placeholder to keep linkers happy in daemon-only build.
// If your main.cpp expects a registration function, keep the name/signature:
void register_multi_account_rpc_handlers() {
    // no-op in daemon-only
}

} // namespace rpc
} // namespace dinero

