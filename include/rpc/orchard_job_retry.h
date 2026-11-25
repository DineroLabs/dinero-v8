#pragma once
// A mining job is built for the tip that was selected when the request
// arrived. Building takes a moment, and a block can arrive meanwhile (on a
// busy chain, or when this node's own miner has just found one): the built
// job is then for a parent that is no longer the tip, and its final recheck
// refuses it. That is not a fault. The job is built once more, for the new
// tip. A failure with the tip unchanged, or a second failure, is reported as
// before.
#include <exception>
#include <optional>
#include <utility>

namespace dinero::rpc {

// `build()` returns the job or throws. `tip_now()` returns the selected tip's
// hash, or nothing if it cannot be read. `rebuilt`, if given, is set when the
// second build was made.
template <class Build, class TipNow, class Hash>
auto BuildOrchardJobOnceMoreIfTipMoved(Build&& build, TipNow&& tip_now, const Hash& tip_before, bool* rebuilt = nullptr)
    -> decltype(build()) {
    if (rebuilt) *rebuilt = false;
    try {
        return build();
    } catch (const std::exception&) {
        const std::optional<Hash> now = tip_now();
        if (!now || *now == tip_before) throw;
        if (rebuilt) *rebuilt = true;
        return build();
    }
}

}  // namespace dinero::rpc
