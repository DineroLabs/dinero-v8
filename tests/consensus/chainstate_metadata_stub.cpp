// Minimal stub for ChainstateMetadata (test-only)
// The full implementation in src/storage/chainstate_metadata.cpp has compile errors
// that are outside Phase H scope. This stub provides just enough for the restart test.

#include "storage/chainstate_metadata.h"
#include "common/status.h"

namespace dinero {

ChainstateMetadata::ChainstateMetadata(const std::filesystem::path& /*dir*/) {}

ChainstateMetadata::~ChainstateMetadata() = default;

StatusOr<ChainstateMetadata::Metadata> ChainstateMetadata::load() {
    // Stub: return empty metadata (test doesn't persist IBD state)
    return Status::NotFound;
}

Status ChainstateMetadata::save(const Metadata& /*metadata*/) {
    // Stub: no-op (test doesn't persist IBD state)
    return Status::Ok;
}

} // namespace dinero
