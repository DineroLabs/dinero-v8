#include "daemon/mempool.h"      // complete type only in the .cpp
#include "daemon/mempool_globals.h"

namespace dinero {
  Mempool* dinero::legacy::g_mempool() = nullptr;  // satisfies the linker; safe default
}
