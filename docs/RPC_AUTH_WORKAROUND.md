# 🔧 RPC Auth Workaround - Quick Fix

**Issue:** RPC cookie auth failing on server (returns "Unauthorized")  
**Root Cause:** TBD (debug build shows auth header is correct)  
**Quick Fix:** Use development mode (disables auth for testing)

---

## ⚡ **Quick Workaround (FOR TESTING ONLY)**

### **Enable Dev Mode:**
```bash
ssh root@96.9.226.98

# Edit service to add -dev flag
sudo nano /etc/systemd/system/dinerod.service

# Change ExecStart line to:
ExecStart=/usr/local/bin/dinerod \
  -datadir=/var/lib/dinero \
  -listen=1 -port=20999 \
  -rpcbind=127.0.0.1 \
  -rpcallowip=127.0.0.1 \
  -externalip=96.9.226.98 \
  -dev

# Restart
sudo systemctl daemon-reload
sudo systemctl restart dinerod

# Test (no auth needed)
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getbestblockhash","params":[]}' \
  http://127.0.0.1:20998/ | jq
```

**⚠️ WARNING:** Dev mode disables auth! Only use for:
- Testing on localhost
- Debugging
- **NEVER on public-facing RPC!**

---

## 🔍 **Proper Fix (To Implement)**

### **Option 1: Check Auth Initialization**

The daemon might not be initializing RpcAuth properly. Check `main_clean.cpp`:

```cpp
// Should have something like:
RpcAuth auth(datadir);
if (!auth.load_cookie()) {
    auth.generate_cookie();
}
http_server.set_auth(&auth);
```

### **Option 2: Debug Cookie Comparison**

Add logging to see exactly what's being compared:

```cpp
// In rpc_auth.cpp validate_request():
std::cerr << "Expected: [" << expected_creds << "]" << std::endl;
std::cerr << "Provided: [" << provided_creds << "]" << std::endl;
std::cerr << "Match: " << (provided_creds == expected_creds) << std::endl;
```

### **Option 3: Disable Auth for Localhost**

```cpp
// In http_rpc_server.cpp process_http_request():
// Check if request is from localhost
std::string client_ip = extract_client_ip(request);
if (client_ip == "127.0.0.1" || client_ip == "::1") {
    // Skip auth for localhost
}
```

---

## 🎯 **Recommended Action**

**For now (testing):**
1. Enable -dev mode temporarily
2. Verify new genesis and premine work
3. Test blockchain functionality

**For production:**
1. Debug auth issue properly
2. Fix cookie comparison
3. Re-deploy without -dev flag

---

## 📝 **Current Status**

**What Works:**
- ✅ Daemon running
- ✅ New genesis initialized
- ✅ P2P listening
- ✅ HTTP server responding

**What Needs Fix:**
- ⚠️ RPC cookie auth validation

**Impact:**
- **Low:** Server is localhost-only anyway
- Can use -dev mode for testing
- Fix properly before opening RPC to network

---

**The daemon is WORKING. Just need to iron out this auth detail!**


