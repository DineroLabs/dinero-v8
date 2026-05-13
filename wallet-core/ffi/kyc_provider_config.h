// OpenKYC Provider Configuration
// Default production API URL for DineroCAN server
// Can be overridden via dinero_wallet_init_kyc_provider() config parameter

#ifndef OPENKYC_DEFAULT_API_URL
#define OPENKYC_DEFAULT_API_URL "https://openkyc.dinero-coin.com"
#endif

// Development fallback (if production URL not available)
#ifndef OPENKYC_DEV_API_URL
#define OPENKYC_DEV_API_URL "http://localhost:8080"
#endif

