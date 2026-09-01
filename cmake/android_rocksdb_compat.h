#pragma once

// Android's <sys/statfs.h> only forward-declares struct statfs for this
// RocksDB release.  <sys/vfs.h> provides the complete type and fstatfs().
#if defined(__ANDROID__)
#include <sys/vfs.h>
#endif
