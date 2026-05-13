// doctor_command.h - CLI entry point for dinerod doctor
// Parses doctor-specific flags, builds context, runs checks, and outputs results.
#pragma once

#include "daemon/doctor/doctor_types.h"
#include <string>

namespace dinero {
namespace doctor {

// Parse "doctor" subcommand arguments and run.
// Returns process exit code (0-3, matching ExitCode contract).
// Called from main() when first positional arg is "doctor".
int RunDoctorCommand(int argc, char* argv[]);

} // namespace doctor
} // namespace dinero
