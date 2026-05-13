#pragma once

namespace dinero {
  class Mempool;                 // forward declare is enough here
  extern Mempool* g_mempool;     // pointer; no complete type required
}
