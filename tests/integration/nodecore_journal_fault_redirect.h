#pragma once

// Forced include for the test subject object ONLY. Parse the real system and
// C++ stream/JSON declarations before defining the redirects: a command-line
// -Dwrite=... would also rename std::ostream::write and Json writer methods.
#include <json/json.h>
#include <filesystem>
#include <sstream>
#include <stdio.h>
#include <unistd.h>

extern "C" int nodecore_test_fsync(int fd);
extern "C" ssize_t nodecore_test_write(int fd, const void* data, size_t size);
extern "C" int nodecore_test_renameat(int from_dir, const char* from, int to_dir, const char* to);

#define fsync nodecore_test_fsync
#define write nodecore_test_write
#define renameat nodecore_test_renameat
