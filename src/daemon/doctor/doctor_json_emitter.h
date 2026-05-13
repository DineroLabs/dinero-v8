// doctor_json_emitter.h - Schema-versioned JSON output (v1.0)
// Stable field ordering. Outputs to stdout or file.
#pragma once

#include "daemon/doctor/doctor_types.h"
#include <ostream>
#include <string>

namespace dinero {
namespace doctor {

class DoctorJsonEmitter {
public:
    // Emit JSON to stream (stdout or file)
    static void Emit(std::ostream& out, const DoctorRunResult& run);

    // Emit JSON to file path. Returns true on success.
    static bool EmitToFile(const std::string& path, const DoctorRunResult& run);
};

} // namespace doctor
} // namespace dinero
