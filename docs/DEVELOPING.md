# Dinero Development Guide

## Thread Lifecycle Contract

**CRITICAL**: All long-lived services must follow this contract to prevent `std::terminate` and memory corruption.

### Service Interface Requirements

Every long-lived service **MUST** expose:
- `start()` - Initialize and start background threads
- `stop()` - Signal shutdown and close resources  
- `join()` - Wait for all threads to complete (or combine with `stop()`)

### Shutdown Order

Components **MUST** shut down in this exact order:
1. **WebSocket Server** - Stop accepting connections, close existing ones
2. **Miner** - Stop mining loops and worker threads
3. **Explorer API** - Stop HTTP handlers
4. **RPC Server** - Stop accepting RPC requests  
5. **Blockchain/Database** - Close database connections

### Thread Management Rules

1. **No `std::thread` may be destroyed while joinable**
   ```cpp
   // ❌ BAD - can cause std::terminate
   ~Service() { /* thread destructor runs while joinable */ }
   
   // ✅ GOOD - always join before destruction
   ~Service() { 
       try { stop(); join(); } catch(...) { /* never throw in dtor */ } 
   }
   ```

2. **All `stop()` methods must be idempotent**
   ```cpp
   void Service::stop() {
       if (stopped_.exchange(true)) return;  // ✅ Idempotent guard
       // ... shutdown logic
   }
   ```

3. **Destructors must never throw**
   ```cpp
   ~Service() {
       try { stop(); join(); } catch(...) { 
           // Log error but never throw from destructor
       }
   }
   ```

### ASIO/WebSocket++ Shutdown Sequence

For WebSocket++ services, follow this **exact** order:

```cpp
void WsServer::stop() {
    // 1. Stop accepting new connections
    server_->stop_listening();
    
    // 2. Close all existing connections gracefully
    for (auto& hdl : connections_) {
        websocketpp::lib::error_code ec;
        server_->close(hdl, websocketpp::close::status::going_away, "shutdown", ec);
        // Ignore errors during shutdown
    }
    
    // 3. Stop the io_service to let run() exit
    server_->get_io_service().stop();
    
    // 4. Join the io thread (CRITICAL - prevents std::terminate)
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    
    // 5. Clean up resources after thread is joined
    server_.reset();
}
```

### Error Handling During Shutdown

Suppress expected shutdown errors in handlers:

```cpp
srv_->set_fail_handler([self = weak_from_this()](auto hdl) {
    if (auto s = self.lock()) {
        auto ec = get_error_code(hdl);
        
        // Suppress expected shutdown errors
        if (s->stopping_.load() && 
            (ec.message().find("Operation aborted") != std::string::npos ||
             ec.message().find("Bad file descriptor") != std::string::npos)) {
            return;  // Expected during shutdown
        }
        
        // Log unexpected errors
        LOG(ERROR) << "WebSocket error: " << ec.message();
    }
});
```

### Memory Safety Rules

1. **Use `weak_ptr` in async callbacks**
   ```cpp
   // ❌ BAD - raw `this` can become dangling
   timer_->async_wait([this](auto ec) { /* use this */ });
   
   // ✅ GOOD - weak_ptr prevents use-after-free
   auto weak_self = weak_from_this();
   timer_->async_wait([weak_self](auto ec) {
       if (auto self = weak_self.lock()) {
           // Safe to use self->...
       }
   });
   ```

2. **Use `thread_local` for multi-threaded static variables**
   ```cpp
   // ❌ BAD - race condition between threads
   static std::string response;
   response = generate_response();
   return response.c_str();
   
   // ✅ GOOD - each thread has its own copy
   thread_local std::string response;
   response = generate_response();
   return response.c_str();
   ```

## Testing Requirements

### Regression Tests

All stability fixes **MUST** have regression tests:

```bash
# Test clean shutdown (prevents std::terminate regression)
./test/e2e/clean_shutdown.sh

# Run with sanitizers to catch memory errors
BIN=./build-asan/bin/dinerod ./test/e2e/clean_shutdown.sh
```

### CI Requirements

All PRs **MUST** pass:
- AddressSanitizer build + clean shutdown test
- UndefinedBehaviorSanitizer (catches integer overflows)
- ThreadSanitizer (on main branch, catches race conditions)

## Common Pitfalls

### 1. WebSocket Timer Lifecycle
```cpp
// ❌ BAD - timer callback can fire after WsServer destruction
std::unique_ptr<asio::steady_timer> timer_;
timer_->async_wait([this](auto ec) { /* this is dangling! */ });

// ✅ GOOD - use weak_ptr and cancel timer before destruction
auto weak_self = weak_from_this();
timer_->async_wait([weak_self](auto ec) {
    if (auto self = weak_self.lock()) { /* safe */ }
});
// In stop(): timer_->cancel(); then join thread
```

### 2. Static Variables in Multi-threaded Code
```cpp
// ❌ BAD - multiple threads writing to same static string
static std::string response;
response = handle_request();  // Race condition!

// ✅ GOOD - thread-local storage
thread_local std::string response;
response = handle_request();  // Each thread has its own copy
```

### 3. Integer Overflow in Parsers
```cpp
// ❌ BAD - can overflow with malicious input
int val = 0;
val = (val << 6) | decode_char(c);  // Integer overflow!

// ✅ GOOD - use appropriate types and check bounds
uint32_t val = 0;
if (val > (UINT32_MAX >> 6)) return error;  // Check before shift
val = (val << 6) | decode_char(c);
```

## Build Configurations

### Debug + Sanitizers (Development)
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON
cmake --build build-asan -j8
```

### Release (Production)
```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF
cmake --build build-release -j8
```

### Thread Sanitizer (Race Detection)
```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j8
```

## Debugging Memory Issues

### AddressSanitizer Output
```
==12345==ERROR: AddressSanitizer: attempting double-free on 0x...
```
- **Cause**: Same memory freed twice (race condition or logic error)
- **Fix**: Use RAII, smart pointers, or add guards

### UndefinedBehaviorSanitizer Output  
```
runtime error: left shift of 400021723 by 6 places cannot be represented in type 'int'
```
- **Cause**: Integer overflow
- **Fix**: Use larger types (`uint32_t`) and bounds checking

### std::terminate
```
[1] + abort ./dinerod
```
- **Cause**: Thread destroyed while joinable
- **Fix**: Always `join()` threads before destruction

Following these guidelines prevents the critical stability issues that plagued earlier versions of the daemon.
