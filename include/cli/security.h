#pragma once
#include <string>

namespace dinero::cli {

enum class CookiePermStatus {
    Ok,
    NotFound,
    NotRegularFile,
    WrongOwner,
    TooPermissive,
    UnknownError
};

// Returns status + a human message if not OK.
std::pair<CookiePermStatus, std::string> CheckCookiePermissions(const std::string& path);

} // namespace dinero::cli
