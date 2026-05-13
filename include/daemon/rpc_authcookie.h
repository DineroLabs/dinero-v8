#pragma once
#include <string>

std::string GetAuthCookiePath(const std::string& datadir, const std::string& rpccookiefileOpt);
bool WriteAuthCookie(const std::string& path, std::string& out_user, std::string& out_pass);
bool ReadAuthCookie(const std::string& path, std::string& out_user, std::string& out_pass);
bool RemoveAuthCookie(const std::string& path);
