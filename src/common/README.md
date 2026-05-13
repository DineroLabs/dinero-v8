# 🏗️ Dinero Common Module

## 🎯 **Overview**

The **Common Module** is the shared code library that provides core functionality used by all Dinero components:

- **🧱 Core Components**: SHA256d, RocksDBManager, RPCClient
- **🛠️ Utilities**: Time, String, File, Network, System, Math, Validation
- **🔗 Integration**: All components link against `dinero_common`

## 📦 **Shared Components**

### **1. SHA256d Implementation** 🔐

**Location**: `include/common/sha256d.h`, `src/common/sha256d.cpp`

**Features**:
- ✅ **Bitcoin-compatible**: Double SHA256 implementation
- ✅ **High Performance**: Optimized C++ implementation
- ✅ **Thread-safe**: Safe for multi-threaded mining
- ✅ **Utility Functions**: Hex conversion, byte manipulation

**Usage**:
```cpp
#include "common/sha256d.h"

using namespace Dinero::Common;

// Double SHA256 hash
std::string hash = double_sha256("Hello, Dinero!");

// SHA256 class for streaming
SHA256 sha256;
sha256.update(data, data_size);
std::vector<uint8_t> result = sha256.finalize();
```

### **2. RPC Client** 🌐

**Location**: `include/common/rpc_client.h`, `src/common/rpc_client.cpp`

**Features**:
- ✅ **Full RPC Support**: All Dinero RPC methods
- ✅ **Thread-safe**: Safe for concurrent access
- ✅ **Error Handling**: Comprehensive error reporting
- ✅ **Configuration**: Flexible RPC configuration

**Usage**:
```cpp
#include "common/rpc_client.h"

using namespace Dinero::Common;

// Create RPC client
RPCConfig config;
config.url = "http://127.0.0.1:8332";
config.user = "dinero_user";
config.pass = "dinero_pass";

RPCClient client(config);

// Make RPC calls
Json::Value info = client.getblockchaininfo();
Json::Value balance = client.getbalance();
Json::Value address = client.getnewaddress();
```

### **3. RocksDB Manager** 💾

**Location**: `include/common/rocksdb_manager.h`, `src/common/rocksdb_manager.cpp`

**Features**:
- ✅ **Thread-safe**: Mutex-protected database access
- ✅ **Miner Analytics**: Performance tracking and heartbeat
- ✅ **Export Features**: JSON export capabilities
- ✅ **Read-only Mode**: Safe for monitoring tools

**Usage**:
```cpp
#include "common/rocksdb_manager.h"

using namespace Dinero::Common;

// Initialize database
RocksDBManager db;
db.initDatabase("/var/lib/dinero/stats", false, "miner_01");

// Store mining stats
db.updateMiningStats(1000000, 5, 150000.0);

// Send heartbeat
db.sendHeartbeat("192.168.1.100", 4, 12345);
```

### **4. Utilities** 🛠️

**Location**: `include/common/utils.h`, `src/common/utils.cpp`

**Features**:
- ✅ **Time Utils**: Timestamp handling, sleep functions
- ✅ **String Utils**: Split, join, trim, case conversion
- ✅ **File Utils**: File operations, directory management
- ✅ **Network Utils**: IP detection, port checking
- ✅ **System Utils**: CPU count, memory usage, OS info
- ✅ **Math Utils**: Hash rate calculation, target conversion
- ✅ **Validation Utils**: Address, hash, hex validation

**Usage**:
```cpp
#include "common/utils.h"

using namespace Dinero::Common;

// Time utilities
uint64_t timestamp = TimeUtils::getCurrentTimestamp();
TimeUtils::sleep(1000); // 1 second

// String utilities
std::vector<std::string> parts = StringUtils::split("a,b,c", ',');
std::string joined = StringUtils::join(parts, "-");

// System utilities
int cpu_count = SystemUtils::getCPUCount();
std::string os_info = SystemUtils::getOSInfo();

// Validation utilities
bool valid_address = ValidationUtils::isValidAddress("hc1...");
bool valid_hash = ValidationUtils::isValidHash("0000...");
```

## 🔗 **Integration**

### **Linking with Common Library**

All components automatically link against the common library:

```cmake
# In component CMakeLists.txt
target_link_libraries(component_name
    PRIVATE
        dinero_common
        # other dependencies...
)
```

### **Include Paths**

Common headers are automatically available:

```cpp
// All components can include common headers
#include "common/sha256d.h"
#include "common/rpc_client.h"
#include "common/rocksdb_manager.h"
#include "common/utils.h"
```

## 🎯 **Benefits**

### **For Developers** 👨‍💻
- ✅ **DRY Principle**: No code duplication across components
- ✅ **Consistent API**: Unified interfaces across all components
- ✅ **Easy Maintenance**: Single source of truth for shared code
- ✅ **Type Safety**: Strong typing with C++ namespaces

### **For Users** 🎯
- ✅ **Reliable**: Battle-tested shared implementations
- ✅ **Consistent**: Same behavior across all components
- ✅ **Efficient**: Optimized shared code
- ✅ **Maintainable**: Easy to update and improve

### **For Production** 🏭
- ✅ **Reduced Size**: Smaller binaries due to shared code
- ✅ **Better Performance**: Optimized shared implementations
- ✅ **Easier Testing**: Test shared code once, use everywhere
- ✅ **Simplified Deployment**: Fewer dependencies to manage

## 🏗️ **Architecture**

### **Namespace Organization**
```cpp
namespace Dinero {
    namespace Common {
        // All shared code goes here
        class SHA256 { ... };
        class RPCClient { ... };
        class RocksDBManager { ... };
        class TimeUtils { ... };
        // ...
    }
}
```

### **Dependency Management**
```
dinero_common
├── CURL::libcurl          # HTTP/RPC communication
├── JSONCPP_LIBRARIES      # JSON parsing
├── ROCKSDB_LIBRARIES      # Database storage
└── pthread                # Threading support
```

### **Thread Safety**
- ✅ **RocksDBManager**: Mutex-protected database access
- ✅ **RPCClient**: Thread-safe HTTP requests
- ✅ **SHA256**: Stateless, thread-safe hashing
- ✅ **Utils**: Stateless utility functions

## 🚀 **Usage Examples**

### **Complete Mining Example**
```cpp
#include "common/sha256d.h"
#include "common/rpc_client.h"
#include "common/rocksdb_manager.h"
#include "common/utils.h"

using namespace Dinero::Common;

int main() {
    // Initialize RPC client
    RPCConfig config;
    config.url = "http://127.0.0.1:8332";
    RPCClient client(config);
    
    // Initialize database
    RocksDBManager db;
    db.initDatabase("./mining_stats", false, "miner_01");
    
    // Get block template
    Json::Value template_data = client.getblocktemplate("hc1...");
    
    // Mine block
    std::string block_data = "...";
    std::string hash = double_sha256(block_data);
    
    // Store stats
    db.updateMiningStats(1000000, 1, 150000.0);
    
    // Submit block
    Json::Value result = client.submitblock(block_data);
    
    return 0;
}
```

### **Utility Functions Example**
```cpp
#include "common/utils.h"

using namespace Dinero::Common;

void example() {
    // Time utilities
    uint64_t now = TimeUtils::getCurrentTimestamp();
    std::string formatted = TimeUtils::formatTimestamp(now);
    
    // String utilities
    std::vector<std::string> parts = StringUtils::split("a,b,c", ',');
    std::string joined = StringUtils::join(parts, "-");
    
    // System utilities
    int cores = SystemUtils::getCPUCount();
    std::string os = SystemUtils::getOSInfo();
    
    // Validation utilities
    bool valid = ValidationUtils::isValidAddress("hc1...");
}
```

## 🎊 **Conclusion**

The **Common Module** is the foundation of the Dinero unified build system:

- **🏗️ Shared Foundation**: All components build on the same core code
- **🎯 Consistent API**: Unified interfaces across the entire system
- **🚀 High Performance**: Optimized implementations for production use
- **🔧 Easy Maintenance**: Single source of truth for shared functionality

**This is how professional, enterprise-grade blockchain systems are built!** 🎯 