#include "cli/security.h"
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace dinero::cli {

std::pair<CookiePermStatus, std::string> CheckCookiePermissions(const std::string& path) {
#if defined(_WIN32)
    // Minimal Windows guard: ensure file exists and is not a directory.
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return {CookiePermStatus::NotFound, "Cookie file not found"};
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        return {CookiePermStatus::NotRegularFile, "Cookie file is a directory"};
    }
    // Deep ACL checks could be added later; for now treat as OK.
    return {CookiePermStatus::Ok, ""};
#else
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return {CookiePermStatus::NotFound, "Cookie file not found"};
    }
    if (!S_ISREG(st.st_mode)) {
        return {CookiePermStatus::NotRegularFile, "Cookie file must be a regular file"};
    }
    uid_t uid = getuid();
    if (st.st_uid != uid) {
        return {CookiePermStatus::WrongOwner, "Cookie file must be owned by the current user"};
    }
    // Mode must be 0600 (owner rw only). Reject any group/other bits.
    if ((st.st_mode & 0077) != 0) {
        return {CookiePermStatus::TooPermissive, "Cookie file permissions too open (expected 0600)"};
    }
    return {CookiePermStatus::Ok, ""};
#endif
}

} // namespace dinero::cli
