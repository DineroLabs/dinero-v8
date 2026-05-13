#pragma once

namespace dinero {
namespace cli {

// Display CLI version information
// Returns: 0 on success
int cmd_version();

// Display comprehensive build information including git SHA, compiler, platform
// Returns: 0 on success
int cmd_buildinfo();

} // namespace cli
} // namespace dinero
