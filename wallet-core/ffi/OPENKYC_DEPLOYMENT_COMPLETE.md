# OpenKYC Backend Deployment & Testing Guide

## ✅ Status: OpenKYC Backend Cloned

The OpenKYC backend repository has been cloned to `wallet-core/openkyc-backend/`.

## 📋 Backend Structure

Based on the cloned repository, OpenKYC appears to be a **Firebase Cloud Functions** project:

- **`idkit_cloud_function/`** - Firebase Cloud Functions backend
- **`app/`** - Flutter web app (frontend)
- **`admin/`** - Admin panel (Flutter web app)

## 🚀 Deployment Options

### Option 1: Firebase Cloud Functions (Recommended)

```bash
cd wallet-core/openkyc-backend/idkit_cloud_function

# Install dependencies
npm install

# Deploy to Firebase
firebase deploy --only functions

# Note: Requires Firebase project setup
```

### Option 2: Local Development Server

The OpenKYC backend may need to be run locally. Check the main README for setup:

```bash
cd wallet-core/openkyc-backend
cat README.md  # Check for setup instructions
```

### Option 3: Docker (if available)

```bash
cd wallet-core/openkyc-backend
docker-compose up -d
```

## ⚙️ API Configuration

Once deployed, configure the API endpoint:

### For Local Development:
```cpp
dinero_wallet_init_kyc_provider("openkyc", "api_url=http://localhost:8080");
```

### For Firebase Cloud Functions:
```cpp
dinero_wallet_init_kyc_provider("openkyc", "api_url=https://your-project.cloudfunctions.net/api");
```

## 🧪 Testing Integration

### Build Test Executable

```bash
cd build
cmake --build . --target test_openkyc_integration
```

### Run Test

```bash
# Test with default localhost
./bin/test_openkyc_integration

# Test with custom URL
./bin/test_openkyc_integration http://localhost:8080

# Test with API key
./bin/test_openkyc_integration http://localhost:8080 your_api_key
```

### Expected Test Flow

1. ✅ Wallet initialization
2. ✅ OpenKYC provider initialization
3. ✅ Provider name verification
4. ✅ Initial status check
5. ✅ Start verification (creates session)
6. ✅ Open verification URL in browser
7. ✅ Complete verification
8. ✅ Check status (should show verified)

## 🔧 Integration Enhancements

### ✅ Completed

- ✅ **JSON Parsing**: Enhanced with jsoncpp (fallback to simple parsing)
- ✅ **Integration Test**: Created test executable
- ✅ **Build System**: Added test target to CMakeLists.txt

### 📝 API Endpoints Required

The OpenKYC backend should expose:

1. **`POST /api/v1/sessions`**
   - Request: `{"user_id": "...", "level": "...", "country": "...", "callback_url": "..."}`
   - Response: `{"session_id": "...", "verification_url": "...", "status": "pending"}`

2. **`GET /api/v1/sessions/{session_id}`**
   - Response: `{"session_id": "...", "status": "approved", "level": "...", "country": "...", "verified_at": ..., "expires_at": ...}`

3. **`GET /api/v1/users/{user_id}/status`** (optional)
   - Same response format as session status

## 🔍 Troubleshooting

### Backend Not Responding

```bash
# Check if backend is running
curl http://localhost:8080/health

# Check logs
cd wallet-core/openkyc-backend
tail -f logs/*.log
```

### Connection Errors

- Verify API URL is correct
- Check CORS settings (allow mobile app origins)
- Ensure firewall allows connections

### JSON Parsing Errors

- Check API response format matches expected schema
- Verify Content-Type header is `application/json`
- Check jsoncpp linkage in build

## 📚 Next Steps

1. **Deploy Backend**: Set up Firebase Cloud Functions or local server
2. **Configure API**: Update API URL in wallet configuration
3. **Run Tests**: Execute integration test to verify connectivity
4. **Mobile Integration**: Connect mobile app to OpenKYC provider

## 🎯 Production Considerations

- **Security**: Use HTTPS for API endpoints
- **Authentication**: Implement API key validation
- **Rate Limiting**: Add rate limiting to prevent abuse
- **Monitoring**: Set up logging and monitoring
- **Error Handling**: Comprehensive error responses

---

**Status**: Backend cloned, integration test ready, JSON parsing enhanced! 🚀

