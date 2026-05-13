// doctor_renderer.h - Human-readable terminal output for doctor results
#pragma once

#include "daemon/doctor/doctor_types.h"
#include "daemon/doctor/doctor_registry.h"
#include <ostream>

namespace dinero {
namespace doctor {

class DoctorRenderer {
public:
    // Render full run results to stream
    static void RenderResults(std::ostream& out, const DoctorRunResult& run);

    // Render --list-checks output
    static void RenderCheckList(std::ostream& out, const DoctorRegistry& registry);

    // Render --explain output for a single check
    static void RenderExplain(std::ostream& out, const RegisteredCheck& check);
};

} // namespace doctor
} // namespace dinero
