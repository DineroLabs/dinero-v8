# Session Management System

**Version:** 1.0
**Date:** 2025-11-03
**Status:** ✅ Production-Ready

---

## Overview

The Session Management System provides **complete visibility and control** over all devices and applications accessing your Dinero wallet. Similar to "Connected Devices" features in Google, GitHub, and Dropbox, users can:

- 📱 See all connected devices with friendly names
- 🔍 View session details (IP, platform, last used, created date)
- ✏️ Rename devices for easy identification
- 🚫 Instantly disconnect specific devices
- 📊 Track session activity and token expiry

---

## Key Features

### 1. Device Tracking
Every token gets a unique session with metadata:
- **Device Name**: User-friendly identifier ("Desktop Wallet", "Trading Bot")
- **IP Address**: Client IP for security auditing
- **Platform**: Detected from User-Agent (Windows, macOS, Linux, Mobile)
- **Timestamps**: Created date, last used time, expiry
- **Scope**: Permission level (wallet, mining, read, admin)

### 2. Session Listing
View all active sessions with complete metadata:
```bash
dinero-cli auth.sessions.list
```

Returns:
- Session ID (UUID format)
- Device name
- IP address
- Platform
- Scope
- Last used time
- Time since last use
- Expiry information

### 3. Session Renaming
Change device names for easy identification:
```bash
dinero-cli auth.sessions.rename <session_id> "My New Device Name"
```

### 4. Selective Revocation
Disconnect specific devices instantly:
```bash
dinero-cli auth.sessions.revoke <session_id>
```

This revokes both the session AND the associated token.

---

## API Reference

### `auth.requesttoken` (Enhanced)

Generate a new token with optional device name.

**Parameters:**
```json
{
  "scope": "wallet",              // Optional: wallet, mining, read, admin
  "auto_refresh": true,            // Optional: Auto-extend lifetime
  "persistent": false,             // Optional: Never expires
  "lifetime": 3600,                // Optional: Seconds (60-86400)
  "device_name": "Desktop Wallet"  // Optional: Device identifier
}
```

**Response:**
```json
{
  "token": "gQjIKkaE2OrwEg9rA8FEqtfkP8htqu",
  "session_id": "b8fef499-f063-43c6-9067-44253299c973",
  "expires_in": 3600,
  "scope": "wallet",
  "auto_refresh": true,
  "persistent": false,
  "device_name": "Desktop Wallet"
}
```

**Example:**
```bash
# Create session with device name
dinero-cli auth.requesttoken '{"device_name":"My Desktop","scope":"wallet"}'

# Create session without device name (defaults to "Unnamed Device")
dinero-cli auth.requesttoken
```

---

### `auth.sessions.list`

List all active sessions with metadata.

**Parameters:** None

**Response:**
```json
{
  "sessions": [
    {
      "session_id": "b8fef499-f063-43c6-9067-44253299c973",
      "device_name": "Desktop Wallet",
      "ip_address": "127.0.0.1",
      "platform": "macOS",
      "scope": "wallet",
      "persistent": false,
      "created_at": 1762175317,
      "last_used": 1762175317,
      "last_used_ago": 0,
      "expires_at": 1762178917,
      "expires_in": 3600
    }
  ],
  "total_sessions": 1
}
```

**Example:**
```bash
# List all sessions
dinero-cli auth.sessions.list

# Format nicely with jq
dinero-cli auth.sessions.list | jq '.sessions[] | {device_name, scope, last_used_ago}'
```

---

### `auth.sessions.revoke`

Revoke a session (disconnect device).

**Parameters:**
```json
{
  "session_id": "b8fef499-f063-43c6-9067-44253299c973"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Session revoked successfully"
}
```

**Example:**
```bash
# Revoke by session ID
dinero-cli auth.sessions.revoke "b8fef499-f063-43c6-9067-44253299c973"
```

---

### `auth.sessions.rename`

Rename a session (change device name).

**Parameters:**
```json
{
  "session_id": "b8fef499-f063-43c6-9067-44253299c973",
  "device_name": "New Device Name"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Session renamed successfully",
  "new_name": "New Device Name"
}
```

**Example:**
```bash
# Rename session
dinero-cli auth.sessions.rename "b8fef499-f063-43c6-9067-44253299c973" "My Updated Name"
```

---

## Usage Examples

### Scenario 1: Desktop Wallet Setup

```bash
# Step 1: Create persistent session for desktop wallet
RESPONSE=$(dinero-cli auth.requesttoken '{
  "device_name": "Desktop Wallet",
  "scope": "wallet",
  "persistent": true
}')

# Extract token and session ID
TOKEN=$(echo "$RESPONSE" | jq -r '.token')
SESSION_ID=$(echo "$RESPONSE" | jq -r '.session_id')

echo "Token: $TOKEN"
echo "Session ID: $SESSION_ID"

# Step 2: Use token for RPC calls
# (Configure wallet to use this token)

# Step 3: View active sessions
dinero-cli auth.sessions.list
```

### Scenario 2: Trading Bot with Limited Scope

```bash
# Create read-only session for trading bot
dinero-cli auth.requesttoken '{
  "device_name": "Trading Bot v2.1",
  "scope": "read",
  "lifetime": 86400
}'

# Bot can now query blockchain data but cannot send transactions
```

### Scenario 3: Revoking Compromised Device

```bash
# Step 1: List all sessions
dinero-cli auth.sessions.list

# Step 2: Find suspicious session
# Look for unfamiliar device names or IPs

# Step 3: Revoke session
dinero-cli auth.sessions.revoke "<suspicious_session_id>"

# Step 4: Verify removal
dinero-cli auth.sessions.list
```

### Scenario 4: Organizing Multiple Devices

```bash
# Create sessions for different devices
dinero-cli auth.requesttoken '{"device_name":"Home Desktop","scope":"wallet","persistent":true}'
dinero-cli auth.requesttoken '{"device_name":"Work Laptop","scope":"read"}'
dinero-cli auth.requesttoken '{"device_name":"Mobile App","scope":"wallet"}'
dinero-cli auth.requesttoken '{"device_name":"Mining Rig","scope":"mining","persistent":true}'

# View all devices
dinero-cli auth.sessions.list | jq '.sessions[] | {device_name, scope, platform}'
```

---

## Integration Examples

### Python Client with Session Management

```python
import requests
import json

class DineroClient:
    def __init__(self, host="http://localhost:8998", device_name="Python Client"):
        self.host = host
        self.token = None
        self.session_id = None
        self.device_name = device_name
        self.request_token()

    def request_token(self):
        """Request token with device name"""
        response = self.call_without_auth("auth.requesttoken", {
            "device_name": self.device_name,
            "scope": "wallet",
            "auto_refresh": True
        })
        self.token = response["token"]
        self.session_id = response["session_id"]
        print(f"✅ Session created: {self.session_id[:8]}...")
        print(f"   Device: {self.device_name}")

    def call_without_auth(self, method, params=None):
        """Call RPC without authentication"""
        response = requests.post(self.host, json={
            "jsonrpc": "2.0",
            "id": 1,
            "method": method,
            "params": params or {}
        })
        return response.json()["result"]

    def call(self, method, params=None):
        """Call RPC with authentication"""
        response = requests.post(self.host,
            headers={"Authorization": f"Bearer {self.token}"},
            json={
                "jsonrpc": "2.0",
                "id": 1,
                "method": method,
                "params": params or {}
            })
        return response.json()["result"]

    def list_sessions(self):
        """List all active sessions"""
        return self.call("auth.sessions.list")

    def revoke_session(self, session_id):
        """Revoke a specific session"""
        return self.call("auth.sessions.revoke", {"session_id": session_id})

# Usage
client = DineroClient(device_name="Python Trading Bot")
balance = client.call("getbalance")
print(f"Balance: {balance}")

# View all connected devices
sessions = client.list_sessions()
for session in sessions["sessions"]:
    print(f"- {session['device_name']} (last used: {session['last_used_ago']}s ago)")
```

### JavaScript/Node.js Session Manager

```javascript
const fetch = require('node-fetch');

class DineroSessionManager {
    constructor(host = 'http://localhost:8998') {
        this.host = host;
        this.token = null;
        this.sessionId = null;
    }

    async createSession(deviceName, scope = 'wallet') {
        const response = await this.rpcCall('auth.requesttoken', {
            device_name: deviceName,
            scope: scope,
            auto_refresh: true
        });

        this.token = response.token;
        this.sessionId = response.session_id;

        console.log(`✅ Session created: ${this.sessionId.substring(0, 8)}...`);
        console.log(`   Device: ${deviceName}`);

        return { token: this.token, sessionId: this.sessionId };
    }

    async listSessions() {
        return await this.rpcCall('auth.sessions.list');
    }

    async revokeSession(sessionId) {
        return await this.rpcCall('auth.sessions.revoke', { session_id: sessionId });
    }

    async renameSession(sessionId, newName) {
        return await this.rpcCall('auth.sessions.rename', {
            session_id: sessionId,
            device_name: newName
        });
    }

    async rpcCall(method, params = {}) {
        const headers = { 'Content-Type': 'application/json' };
        if (this.token) {
            headers['Authorization'] = `Bearer ${this.token}`;
        }

        const response = await fetch(this.host, {
            method: 'POST',
            headers: headers,
            body: JSON.stringify({
                jsonrpc: '2.0',
                id: 1,
                method: method,
                params: params
            })
        });

        const result = await response.json();
        return result.result;
    }
}

// Usage
(async () => {
    const manager = new DineroSessionManager();

    // Create session
    await manager.createSession('Node.js Desktop App');

    // List all sessions
    const sessions = await manager.listSessions();
    console.log(`\nActive sessions: ${sessions.total_sessions}`);
    sessions.sessions.forEach(s => {
        console.log(`- ${s.device_name} (${s.scope})`);
    });
})();
```

---

## Security Considerations

### Session Limits

Prevent too many concurrent sessions per scope:

```cpp
// In C++ code (example)
auto& sm = SessionManager::instance();
sm.SetSessionLimit("wallet", 10);  // Max 10 wallet sessions
sm.SetSessionLimit("mining", 5);   // Max 5 mining sessions
sm.SetSessionLimit("admin", 2);    // Max 2 admin sessions
// 0 = unlimited (default)
```

### IP Locking

Sessions track client IP addresses. When a token is validated, the IP must match:

```cpp
bool valid = TokenManager::instance().ValidateToken(token, current_ip, required_scope);
```

If IP doesn't match → validation fails.

### Platform Detection

Platform is extracted from User-Agent header:
- **Windows**: "Windows NT", "Win32", "Win64"
- **macOS**: "Mac OS", "macOS", "Darwin"
- **Linux**: "Linux", "X11"
- **Mobile**: "Mobile", "Android", "iPhone", "iPad"
- **Unknown**: No match or empty User-Agent

---

## Architecture

### Data Flow

```
1. Client calls auth.requesttoken with device_name
   ↓
2. TokenManager generates 32-char token
   ↓
3. SessionManager creates session record
   - Generates UUID session_id
   - Extracts platform from User-Agent
   - Stores all metadata
   ↓
4. Response includes both token and session_id
   ↓
5. Client uses token for subsequent requests
   ↓
6. TokenManager validates token
   ↓
7. SessionManager updates last_used timestamp
```

### Session Lifecycle

```
Created → Active → Expired/Revoked → Deleted
   ↓         ↓            ↓
  UUID    Tracked     Removed from registry
```

### Database Schema (In-Memory)

```cpp
struct SessionInfo {
    std::string session_id;        // UUID
    std::string token;              // Associated token
    std::string device_name;        // User-friendly name
    std::string ip_address;         // Client IP
    std::string user_agent;         // HTTP User-Agent
    std::string platform;           // Detected platform
    std::string scope;              // Permission level
    bool persistent;                // Never expires?
    time_point created_at;          // Creation timestamp
    time_point last_used;           // Last activity
    time_point expires_at;          // Expiry time
};
```

---

## Testing

### Run Session Management Tests

```bash
# Full test suite
./test_sessions.sh
```

**Expected output:**
```
✅ Session creation with device names
✅ Session listing with metadata
✅ Session renaming
✅ Session revocation
✅ Timestamp tracking
```

### Manual Testing

```bash
# Start daemon
./build/dinerod --regtest --daemon

# Create test sessions
dinero-cli auth.requesttoken '{"device_name":"Test Device 1"}'
dinero-cli auth.requesttoken '{"device_name":"Test Device 2"}'
dinero-cli auth.requesttoken '{"device_name":"Test Device 3"}'

# List sessions
dinero-cli auth.sessions.list

# Rename session
SESSION_ID=$(dinero-cli auth.sessions.list | jq -r '.sessions[0].session_id')
dinero-cli auth.sessions.rename "$SESSION_ID" "Renamed Device"

# Revoke session
dinero-cli auth.sessions.revoke "$SESSION_ID"

# Verify removal
dinero-cli auth.sessions.list
```

---

## FAQ

**Q: What happens when I revoke a session?**
A: Both the session AND the associated token are immediately invalidated. Any requests using that token will fail with 401 Unauthorized.

**Q: Can I rename a session after creation?**
A: Yes! Use `auth.sessions.rename` to change the device name at any time.

**Q: How many sessions can I have?**
A: By default, unlimited. Administrators can set limits per scope using `SessionManager::SetSessionLimit()`.

**Q: What if I don't provide a device_name?**
A: The session will be created with the default name "Unnamed Device". You can rename it later.

**Q: Are sessions persistent across daemon restarts?**
A: Currently sessions are stored in memory. They will be lost on daemon restart. Future versions may add persistence.

**Q: Can I export session data?**
A: Yes, use `auth.sessions.list` and pipe to `jq` or save to a file:
```bash
dinero-cli auth.sessions.list > sessions.json
```

**Q: How do I find my session_id?**
A: It's returned when you create a token:
```bash
dinero-cli auth.requesttoken | jq -r '.session_id'
```

Or list all sessions:
```bash
dinero-cli auth.sessions.list | jq '.sessions[] | {session_id, device_name}'
```

---

## Roadmap

### Future Enhancements

- [ ] **Persistent Sessions**: Save sessions to database, survive daemon restarts
- [ ] **Session Activity Log**: Track all RPC calls per session
- [ ] **Geolocation**: Display approximate location from IP address
- [ ] **Device Types**: Icons for different device types (desktop, mobile, server)
- [ ] **Automatic Cleanup**: Remove expired sessions automatically
- [ ] **Session Groups**: Organize sessions by user or purpose
- [ ] **Notification System**: Alert when new device connects
- [ ] **Two-Factor Auth**: Require 2FA for sensitive sessions

---

## Summary

The Session Management System provides **enterprise-grade visibility and control** over connected devices. Key benefits:

✅ **User Control**: See and manage all connected devices
✅ **Security**: Instantly disconnect suspicious sessions
✅ **Organization**: Name devices for easy identification
✅ **Auditing**: Track session activity and timestamps
✅ **Integration**: Full API for wallet and application integration

**Ready for production use.**

---

**Implementation Files:**
- `include/rpc/session_manager.h` - SessionManager interface
- `src/rpc/session_manager.cpp` - SessionManager implementation
- `src/rpc/methods_auth.cpp` - Session RPC methods (list, revoke, rename)
- `docs/SESSION_MANAGEMENT.md` - This document

**Test Files:**
- `test_sessions.sh` - Session management test suite

**Related Documentation:**
- `docs/TOKEN_AUTH_COMPLETE.md` - Token authentication system
- `docs/RPC_API.md` - Complete RPC API reference
