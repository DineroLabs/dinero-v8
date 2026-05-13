// FFI Bridge to Dinero C++ Wallet Core
// This module provides Rust bindings to libdinero_wallet.a
// 
// IMPORTANT: This app is FULLY STANDALONE:
// - All wallet operations run locally on the device
// - C++ library is statically compiled into the app
// - No macOS/Windows/Linux dependency
// - Works offline (only needs internet for blockchain sync)

use std::os::raw::{c_char, c_int, c_ulonglong};
use std::ffi::{CString, CStr};
use std::ptr;

// C FFI types matching C++ wallet interface (FFI_ prefix to avoid conflicts)
#[repr(C)]
pub struct FFI_WalletBalance {
    pub total: f64,
    pub confirmed: f64,
    pub unconfirmed: f64,
    pub immature: f64,
}

#[repr(C)]
pub struct FFI_WalletAddress {
    pub address: *const c_char,
    pub path: *const c_char,
    pub index: c_int,
    pub balance: f64,
}

#[repr(C)]
pub struct FFI_WalletUTXO {
    pub txid: *const c_char,
    pub vout: c_int,
    pub address: *const c_char,
    pub amount: f64,
    pub confirmations: c_int,
    pub spendable: bool,
    pub coinbase: bool,
}

#[repr(C)]
pub struct FFI_QRPayment {
    pub address: [u8; 128],
    pub amount: f64,
    pub label: [u8; 128],
}

#[repr(C)]
pub struct FFI_TransactionNotification {
    pub txid: [u8; 65],
    pub address: [u8; 128],
    pub amount: f64,
    pub confirmations: c_int,
    pub timestamp: i64,
    pub category: [u8; 32],
    pub is_new: bool,
}

#[repr(C)]
pub struct FFI_SyncProgress {
    pub progress: f64,
    pub current_block: c_int,
    pub total_blocks: c_int,
    pub is_syncing: bool,
    pub status_message: *mut c_char,
}

#[repr(C)]
pub enum DineroErrorCode {
    Success = 0,
    ErrorGeneric = -1,
    ErrorWalletNotFound = -2,
    ErrorWalletLocked = -3,
    ErrorWalletEncrypted = -4,
    ErrorInvalidMnemonic = -5,
    ErrorInvalidAddress = -6,
    ErrorInsufficientFunds = -7,
    ErrorInvalidAmount = -8,
    ErrorTxBroadcastFailed = -9,
    ErrorFileIo = -10,
    ErrorInvalidFormat = -11,
    ErrorNetwork = -12,
    ErrorAuthentication = -13,
    ErrorNotImplemented = -14,
}

#[repr(C)]
pub struct FFI_ExchangeRate {
    pub from_symbol: [u8; 8],
    pub to_symbol: [u8; 8],
    pub rate: f64,
    pub to_amount: f64,
    pub min_amount: f64,
    pub max_amount: f64,
    pub timestamp: i64,
    pub provider: [u8; 32],
}

#[repr(C)]
pub struct FFI_SwapTransaction {
    pub txid: [u8; 65],
    pub from_address: [u8; 128],
    pub to_address: [u8; 128],
    pub from_amount: f64,
    pub to_amount: f64,
    pub from_symbol: [u8; 8],
    pub to_symbol: [u8; 8],
    pub fee: f64,
    pub status: [u8; 32],
    pub timestamp: i64,
}

#[repr(C)]
pub struct FFI_LiquidityPool {
    pub pool_id: [u8; 64],
    pub symbol: [u8; 8],
    pub total_liquidity: f64,
    pub available_liquidity: f64,
    pub apy: f64,
    pub min_deposit: f64,
    pub max_deposit: f64,
    pub last_update: i64,
}

#[repr(C)]
pub struct FFI_FiatOrder {
    pub order_id: [u8; 64],
    pub payment_method: [u8; 32],
    pub fiat_amount: f64,
    pub fiat_currency: [u8; 8],
    pub crypto_amount: f64,
    pub crypto_symbol: [u8; 8],
    pub exchange_rate: f64,
    pub fee: f64,
    pub status: [u8; 32],
    pub payment_url: [u8; 256],
    pub expires_at: i64,
    pub created_at: i64,
}

#[repr(C)]
pub struct FFI_KYCStatus {
    pub is_verified: bool,
    pub verification_level: [u8; 32],
    pub provider: [u8; 32],
    pub verified_at: i64,
    pub expires_at: i64,
    pub country: [u8; 3],
}

// Link to libdinero_wallet_ffi.a (built by CMake)
// This library is STATICALLY COMPILED into the mobile app
// No external dependencies - everything runs on-device
#[link(name = "dinero_wallet_ffi", kind = "static")]
extern "C" {
    // Wallet initialization
    // datadir: Local storage path on device (iOS: App Sandbox, Android: App Data)
    fn dinero_wallet_init(datadir: *const c_char) -> c_int;
    fn dinero_wallet_create(mnemonic_out: *mut *mut c_char) -> c_int;
    fn dinero_wallet_restore(mnemonic: *const c_char, passphrase: *const c_char) -> c_int;
    
    // Wallet encryption (all crypto runs LOCALLY on device)
    fn dinero_wallet_encrypt(password: *const c_char) -> c_int;
    fn dinero_wallet_unlock(password: *const c_char, timeout_seconds: c_int) -> c_int;
    fn dinero_wallet_lock() -> c_int;
    fn dinero_wallet_is_encrypted() -> bool;
    fn dinero_wallet_is_locked() -> bool;
    
    // Address operations (100% local, no network needed)
    fn dinero_wallet_get_balance() -> FFI_WalletBalance;
    fn dinero_wallet_get_new_address(label: *const c_char, address_out: *mut *mut c_char) -> c_int;
    fn dinero_wallet_get_change_address(address_out: *mut *mut c_char) -> c_int;
    fn dinero_wallet_get_mining_address(address_out: *mut *mut c_char) -> c_int;
    fn dinero_wallet_set_label(address: *const c_char, label: *const c_char) -> c_int;
    fn dinero_wallet_get_label(address: *const c_char, label_out: *mut *mut c_char) -> c_int;
    
    // Transaction operations
    // Transaction signing happens LOCALLY on device
    // Only broadcast_tx requires network connection
    fn dinero_wallet_send_transaction(
        to: *const c_char,
        amount: f64,
        fee_rate: f64,
        note: *const c_char,
        txid_out: *mut *mut c_char,
    ) -> c_int;
    
    fn dinero_wallet_list_utxos(
        min_confirmations: c_int,
        utxos_out: *mut *mut FFI_WalletUTXO,
        count_out: *mut c_int,
    ) -> c_int;
    
    fn dinero_wallet_list_addresses(
        addresses_out: *mut *mut FFI_WalletAddress,
        count_out: *mut c_int,
    ) -> c_int;
    
    // Backup & recovery (100% local, no network)
    fn dinero_wallet_backup_file(filepath: *const c_char, hash_out: *mut *mut c_char) -> c_int;
    fn dinero_wallet_get_mnemonic(mnemonic_out: *mut *mut c_char) -> c_int;
    
    // Network operations (optional - only for blockchain sync)
    // These connect to DineroCoin network nodes (can be configured)
    fn dinero_wallet_connect_rpc(rpc_url: *const c_char) -> c_int;
    fn dinero_wallet_sync_balance() -> c_int;
    fn dinero_wallet_broadcast_tx(tx_hex: *const c_char, txid_out: *mut *mut c_char) -> c_int;
    
    // Cleanup
    fn dinero_wallet_free_string(ptr: *mut c_char);
    fn dinero_wallet_free_addresses(ptr: *mut FFI_WalletAddress, count: c_int);
    fn dinero_wallet_free_utxos(ptr: *mut FFI_WalletUTXO, count: c_int);
    fn dinero_wallet_free_notifications(ptr: *mut FFI_TransactionNotification, count: c_int);
    
    // Payment UX Features
    fn dinero_wallet_export_transactions(format: *const c_char, dest: *const c_char) -> c_int;
    fn dinero_wallet_export_transactions_batched(
        format: *const c_char,
        dest: *const c_char,
        batch_size: c_int,
        callback: Option<extern "C" fn(*const c_char, f64)>,
    ) -> c_int;
    fn dinero_wallet_get_tx_confirmations(txid: *const c_char, confirmations_out: *mut c_int) -> c_int;
    fn dinero_wallet_parse_uri(uri: *const c_char, out: *mut FFI_QRPayment) -> c_int;
    fn dinero_wallet_generate_uri(
        address: *const c_char,
        amount: f64,
        label: *const c_char,
        uri_out: *mut *mut c_char,
    ) -> c_int;
    fn dinero_wallet_check_new_transactions(
        notifications_out: *mut *mut FFI_TransactionNotification,
        count_out: *mut c_int,
    ) -> c_int;
    
    // Performance & Diagnostics
    fn dinero_wallet_get_sync_progress(progress_out: *mut FFI_SyncProgress) -> c_int;
    fn dinero_wallet_get_last_error() -> DineroErrorCode;
    fn dinero_wallet_get_error_message(error_code: DineroErrorCode, message_out: *mut *mut c_char) -> c_int;
    
    // Security & Key Management
    fn dinero_wallet_store_secure(wallet_data: *const c_char, data_length: usize) -> c_int;
    fn dinero_wallet_retrieve_secure(wallet_data_out: *mut *mut c_char, data_length_out: *mut usize) -> c_int;
    fn dinero_wallet_secure_storage_available() -> bool;
    
    // Exchange & Swap Features
    fn dinero_wallet_get_exchange_rate(
        from: *const c_char,
        to: *const c_char,
        amount: f64,
        rate_out: *mut FFI_ExchangeRate,
    ) -> c_int;
    fn dinero_wallet_create_swap_tx(
        from_address: *const c_char,
        to_address: *const c_char,
        amount: f64,
        from_symbol: *const c_char,
        to_symbol: *const c_char,
        swap_out: *mut FFI_SwapTransaction,
    ) -> c_int;
    fn dinero_wallet_get_swap_status(
        swap_id: *const c_char,
        swap_out: *mut FFI_SwapTransaction,
    ) -> c_int;
    
    // Liquidity & On-Ramp Features
    fn dinero_wallet_get_liquidity_pools(
        pools_out: *mut *mut FFI_LiquidityPool,
        count_out: *mut c_int,
    ) -> c_int;
    fn dinero_wallet_add_liquidity(
        pool_id: *const c_char,
        amount: f64,
        txid_out: *mut *mut c_char,
    ) -> c_int;
    fn dinero_wallet_remove_liquidity(
        pool_id: *const c_char,
        amount: f64,
        txid_out: *mut *mut c_char,
    ) -> c_int;
    fn dinero_wallet_create_fiat_order(
        amount: f64,
        fiat_currency: *const c_char,
        crypto_symbol: *const c_char,
        payment_method: *const c_char,
        order_out: *mut FFI_FiatOrder,
    ) -> c_int;
    fn dinero_wallet_get_fiat_order_status(
        order_id: *const c_char,
        order_out: *mut FFI_FiatOrder,
    ) -> c_int;
    fn dinero_wallet_get_kyc_status(status_out: *mut FFI_KYCStatus) -> c_int;
    fn dinero_wallet_start_kyc_verification(
        level: *const c_char,
        country: *const c_char,
        verification_url_out: *mut *mut c_char,
    ) -> c_int;
    fn dinero_wallet_free_pools(ptr: *mut FFI_LiquidityPool, count: c_int);
}

// Safe Rust wrapper for FFI functions
pub mod wallet {
    use super::*;
    use anyhow::{Result, Context};
    
    // Re-export FFI types for use in commands
    pub use super::FFI_WalletBalance;
    pub use super::FFI_WalletAddress;
    pub use super::FFI_WalletUTXO;
    pub use super::FFI_QRPayment;
    pub use super::FFI_TransactionNotification;
    pub use super::FFI_SyncProgress;
    pub use super::DineroErrorCode;
    pub use super::FFI_ExchangeRate;
    pub use super::FFI_SwapTransaction;
    pub use super::FFI_LiquidityPool;
    pub use super::FFI_FiatOrder;
    pub use super::FFI_KYCStatus;
    
    /// Initialize wallet with local data directory
    /// This is a LOCAL operation - no network required
    /// datadir: Path in app sandbox (iOS) or app data (Android)
    pub fn init(datadir: &str) -> Result<()> {
        let c_datadir = CString::new(datadir)?;
        let result = unsafe { dinero_wallet_init(c_datadir.as_ptr()) };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to initialize wallet"))
        }
    }
    
    /// Create new wallet (100% local, offline-capable)
    /// Returns BIP-39 mnemonic phrase
    pub fn create() -> Result<String> {
        let mut mnemonic_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe { dinero_wallet_create(&mut mnemonic_ptr) };
        if result == 0 && !mnemonic_ptr.is_null() {
            let mnemonic = unsafe {
                CStr::from_ptr(mnemonic_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(mnemonic_ptr) };
            Ok(mnemonic)
        } else {
            Err(anyhow::anyhow!("Failed to create wallet"))
        }
    }
    
    /// Restore wallet from mnemonic (100% local, offline-capable)
    pub fn restore(mnemonic: &str, passphrase: Option<&str>) -> Result<()> {
        let c_mnemonic = CString::new(mnemonic)?;
        let c_passphrase = passphrase
            .map(|s| CString::new(s).unwrap())
            .unwrap_or_else(|| CString::new("").unwrap());
        
        let result = unsafe {
            dinero_wallet_restore(c_mnemonic.as_ptr(), c_passphrase.as_ptr())
        };
        
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to restore wallet"))
        }
    }
    
    /// Encrypt wallet (100% local, offline-capable)
    /// All encryption happens on-device using AES-256-GCM
    pub fn encrypt(password: &str) -> Result<()> {
        let c_password = CString::new(password)?;
        let result = unsafe { dinero_wallet_encrypt(c_password.as_ptr()) };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to encrypt wallet"))
        }
    }
    
    /// Unlock wallet (100% local, offline-capable)
    pub fn unlock(password: &str, timeout_seconds: i32) -> Result<()> {
        let c_password = CString::new(password)?;
        let result = unsafe { dinero_wallet_unlock(c_password.as_ptr(), timeout_seconds) };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to unlock wallet"))
        }
    }
    
    /// Lock wallet (100% local)
    pub fn lock() -> Result<()> {
        let result = unsafe { dinero_wallet_lock() };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to lock wallet"))
        }
    }
    
    pub fn is_encrypted() -> bool {
        unsafe { dinero_wallet_is_encrypted() }
    }
    
    pub fn is_locked() -> bool {
        unsafe { dinero_wallet_is_locked() }
    }
    
    /// Get wallet balance
    /// NOTE: This returns cached balance. For fresh balance, use sync_balance()
    pub fn get_balance() -> Result<FFI_WalletBalance> {
        let balance = unsafe { dinero_wallet_get_balance() };
        Ok(balance)
    }
    
    /// Get new address (100% local, offline-capable)
    pub fn get_new_address(label: Option<&str>) -> Result<String> {
        let c_label = label
            .map(|s| CString::new(s).unwrap())
            .unwrap_or_else(|| CString::new("").unwrap());
        
        let mut address_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe {
            dinero_wallet_get_new_address(c_label.as_ptr(), &mut address_ptr)
        };
        
        if result == 0 && !address_ptr.is_null() {
            let address = unsafe {
                CStr::from_ptr(address_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(address_ptr) };
            Ok(address)
        } else {
            Err(anyhow::anyhow!("Failed to get new address"))
        }
    }
    
    /// Send transaction
    /// Transaction signing happens LOCALLY on device
    /// Returns signed transaction hex (needs to be broadcast via broadcast_tx())
    pub fn send_transaction(
        to: &str,
        amount: f64,
        fee_rate: f64,
        note: Option<&str>,
    ) -> Result<String> {
        let c_to = CString::new(to)?;
        let c_note = note
            .map(|s| CString::new(s).unwrap())
            .unwrap_or_else(|| CString::new("").unwrap());
        
        let mut txid_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe {
            dinero_wallet_send_transaction(
                c_to.as_ptr(),
                amount,
                fee_rate,
                c_note.as_ptr(),
                &mut txid_ptr,
            )
        };
        
        if result == 0 && !txid_ptr.is_null() {
            let txid = unsafe {
                CStr::from_ptr(txid_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(txid_ptr) };
            Ok(txid)
        } else {
            Err(anyhow::anyhow!("Failed to send transaction"))
        }
    }
    
    /// Connect to DineroCoin RPC node (requires internet)
    /// rpc_url: e.g., "https://rpc.dinero-coin.com" or user-configured node
    pub fn connect_rpc(rpc_url: &str) -> Result<()> {
        let c_url = CString::new(rpc_url)?;
        let result = unsafe { dinero_wallet_connect_rpc(c_url.as_ptr()) };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to connect to RPC node"))
        }
    }
    
    /// Sync balance from network (requires internet)
    pub fn sync_balance() -> Result<()> {
        let result = unsafe { dinero_wallet_sync_balance() };
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to sync balance"))
        }
    }
    
    /// Broadcast transaction to network (requires internet)
    /// tx_hex: Signed transaction hex from send_transaction()
    pub fn broadcast_tx(tx_hex: &str) -> Result<String> {
        let c_tx_hex = CString::new(tx_hex)?;
        let mut txid_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe {
            dinero_wallet_broadcast_tx(c_tx_hex.as_ptr(), &mut txid_ptr)
        };
        
        if result == 0 && !txid_ptr.is_null() {
            let txid = unsafe {
                CStr::from_ptr(txid_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(txid_ptr) };
            Ok(txid)
        } else {
            Err(anyhow::anyhow!("Failed to broadcast transaction"))
        }
    }
    
    /// Export transaction history to CSV or JSON
    /// format: "csv" or "json"
    /// dest: File path where export should be written
    pub fn export_transactions(format: &str, dest: &str) -> Result<()> {
        let c_format = CString::new(format)?;
        let c_dest = CString::new(dest)?;
        let result = unsafe {
            dinero_wallet_export_transactions(c_format.as_ptr(), c_dest.as_ptr())
        };
        
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to export transactions"))
        }
    }
    
    /// Export transaction history with progress callback
    pub fn export_transactions_with_progress(
        format: &str,
        dest: &str,
        batch_size: Option<i32>,
        callback: Option<Box<dyn Fn(&str, f64)>>,
    ) -> Result<()> {
        let c_format = CString::new(format)?;
        let c_dest = CString::new(dest)?;
        let batch = batch_size.unwrap_or(0);
        
        // Wrap callback in C-compatible function
        extern "C" fn progress_callback(txid: *const c_char, progress: f64) {
            // This is a simplified version - full implementation would use closure
            if !txid.is_null() {
                let _txid_str = unsafe {
                    CStr::from_ptr(txid).to_string_lossy()
                };
                // In real implementation, would call user-provided callback
            }
        }
        
        let cb_ptr = if callback.is_some() {
            Some(progress_callback as extern "C" fn(*const c_char, f64))
        } else {
            None
        };
        
        let result = unsafe {
            dinero_wallet_export_transactions_batched(
                c_format.as_ptr(),
                c_dest.as_ptr(),
                batch,
                cb_ptr,
            )
        };
        
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to export transactions"))
        }
    }
    
    /// Get transaction confirmation count
    pub fn get_tx_confirmations(txid: &str) -> Result<i32> {
        let c_txid = CString::new(txid)?;
        let mut confirmations: c_int = 0;
        
        let result = unsafe {
            dinero_wallet_get_tx_confirmations(c_txid.as_ptr(), &mut confirmations)
        };
        
        if result == 0 {
            Ok(confirmations as i32)
        } else {
            Err(anyhow::anyhow!("Failed to get confirmations"))
        }
    }
    
    /// Parse Dinero payment URI (dinero:address?amount=X&label=Y)
    /// Returns parsed payment information
    pub fn parse_payment_uri(uri: &str) -> Result<FFI_QRPayment> {
        let c_uri = CString::new(uri)?;
        let mut out = FFI_QRPayment {
            address: [0; 128],
            amount: 0.0,
            label: [0; 128],
        };
        
        let result = unsafe {
            dinero_wallet_parse_uri(c_uri.as_ptr(), &mut out)
        };
        
        if result == 0 {
            Ok(out)
        } else {
            Err(anyhow::anyhow!("Failed to parse payment URI"))
        }
    }
    
    /// Generate Dinero payment URI for QR code
    /// address: Recipient address
    /// amount: Optional amount (0.0 to omit)
    /// label: Optional label
    pub fn generate_payment_uri(
        address: &str,
        amount: Option<f64>,
        label: Option<&str>,
    ) -> Result<String> {
        let c_address = CString::new(address)?;
        let c_label = label
            .map(|s| CString::new(s).unwrap())
            .unwrap_or_else(|| CString::new("").unwrap());
        
        let amount_value = amount.unwrap_or(0.0);
        
        let mut uri_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe {
            dinero_wallet_generate_uri(
                c_address.as_ptr(),
                amount_value,
                c_label.as_ptr(),
                &mut uri_ptr,
            )
        };
        
        if result == 0 && !uri_ptr.is_null() {
            let uri = unsafe {
                CStr::from_ptr(uri_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(uri_ptr) };
            Ok(uri)
        } else {
            Err(anyhow::anyhow!("Failed to generate payment URI"))
        }
    }
    
    /// Check for new transactions since last call
    /// Returns vector of new transaction notifications
    pub fn check_new_transactions() -> Result<Vec<FFI_TransactionNotification>> {
        let mut notifications_ptr: *mut FFI_TransactionNotification = ptr::null_mut();
        let mut count: c_int = 0;
        
        let result = unsafe {
            dinero_wallet_check_new_transactions(&mut notifications_ptr, &mut count)
        };
        
        if result == 0 && !notifications_ptr.is_null() && count > 0 {
            let mut notifications = Vec::new();
            
            unsafe {
                for i in 0..count {
                    notifications.push(*notifications_ptr.add(i as usize));
                }
                
                dinero_wallet_free_notifications(notifications_ptr, count);
            }
            
            Ok(notifications)
        } else {
            Ok(Vec::new()) // No new transactions
        }
    }
    
    /// Get wallet synchronization progress
    pub fn get_sync_progress() -> Result<FFI_SyncProgress> {
        let mut progress = FFI_SyncProgress {
            progress: 0.0,
            current_block: 0,
            total_blocks: 0,
            is_syncing: false,
            status_message: ptr::null_mut(),
        };
        
        let result = unsafe {
            dinero_wallet_get_sync_progress(&mut progress)
        };
        
        if result == 0 {
            Ok(progress)
        } else {
            Err(anyhow::anyhow!("Failed to get sync progress"))
        }
    }
    
    /// Get last error code
    pub fn get_last_error() -> DineroErrorCode {
        unsafe { dinero_wallet_get_last_error() }
    }
    
    /// Get error message for error code
    pub fn get_error_message(error_code: DineroErrorCode) -> Result<String> {
        let mut message_ptr: *mut c_char = ptr::null_mut();
        let result = unsafe {
            dinero_wallet_get_error_message(error_code, &mut message_ptr)
        };
        
        if result == 0 && !message_ptr.is_null() {
            let message = unsafe {
                CStr::from_ptr(message_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(message_ptr) };
            Ok(message)
        } else {
            Err(anyhow::anyhow!("Failed to get error message"))
        }
    }
    
    /// Store encrypted wallet data in platform secure storage
    pub fn store_secure(wallet_data: &str) -> Result<()> {
        let c_data = CString::new(wallet_data)?;
        let result = unsafe {
            dinero_wallet_store_secure(c_data.as_ptr(), wallet_data.len())
        };
        
        if result == 0 {
            Ok(())
        } else {
            Err(anyhow::anyhow!("Failed to store secure data"))
        }
    }
    
    /// Retrieve encrypted wallet data from platform secure storage
    pub fn retrieve_secure() -> Result<String> {
        let mut data_ptr: *mut c_char = ptr::null_mut();
        let mut data_len: usize = 0;
        
        let result = unsafe {
            dinero_wallet_retrieve_secure(&mut data_ptr, &mut data_len)
        };
        
        if result == 0 && !data_ptr.is_null() {
            let data = unsafe {
                CStr::from_ptr(data_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(data_ptr) };
            Ok(data)
        } else {
            Err(anyhow::anyhow!("Failed to retrieve secure data"))
        }
    }
    
    /// Check if platform secure storage is available
    pub fn secure_storage_available() -> bool {
        unsafe { dinero_wallet_secure_storage_available() }
    }
    
    /// Get exchange rate between two currencies
    pub fn get_exchange_rate(
        from: &str,
        to: &str,
        amount: f64,
    ) -> Result<FFI_ExchangeRate> {
        let c_from = CString::new(from)?;
        let c_to = CString::new(to)?;
        let mut rate = FFI_ExchangeRate {
            from_symbol: [0; 8],
            to_symbol: [0; 8],
            rate: 0.0,
            to_amount: 0.0,
            min_amount: 0.0,
            max_amount: 0.0,
            timestamp: 0,
            provider: [0; 32],
        };
        
        let result = unsafe {
            dinero_wallet_get_exchange_rate(c_from.as_ptr(), c_to.as_ptr(), amount, &mut rate)
        };
        
        if result == 0 {
            Ok(rate)
        } else {
            Err(anyhow::anyhow!("Failed to get exchange rate"))
        }
    }
    
    /// Create swap transaction
    pub fn create_swap_tx(
        from_address: &str,
        to_address: &str,
        amount: f64,
        from_symbol: &str,
        to_symbol: &str,
    ) -> Result<FFI_SwapTransaction> {
        let c_from_addr = CString::new(from_address)?;
        let c_to_addr = CString::new(to_address)?;
        let c_from_sym = CString::new(from_symbol)?;
        let c_to_sym = CString::new(to_symbol)?;
        
        let mut swap = FFI_SwapTransaction {
            txid: [0; 65],
            from_address: [0; 128],
            to_address: [0; 128],
            from_amount: 0.0,
            to_amount: 0.0,
            from_symbol: [0; 8],
            to_symbol: [0; 8],
            fee: 0.0,
            status: [0; 32],
            timestamp: 0,
        };
        
        let result = unsafe {
            dinero_wallet_create_swap_tx(
                c_from_addr.as_ptr(),
                c_to_addr.as_ptr(),
                amount,
                c_from_sym.as_ptr(),
                c_to_sym.as_ptr(),
                &mut swap,
            )
        };
        
        if result == 0 {
            Ok(swap)
        } else {
            Err(anyhow::anyhow!("Failed to create swap transaction"))
        }
    }
    
    /// Get swap transaction status
    pub fn get_swap_status(swap_id: &str) -> Result<FFI_SwapTransaction> {
        let c_swap_id = CString::new(swap_id)?;
        let mut swap = FFI_SwapTransaction {
            txid: [0; 65],
            from_address: [0; 128],
            to_address: [0; 128],
            from_amount: 0.0,
            to_amount: 0.0,
            from_symbol: [0; 8],
            to_symbol: [0; 8],
            fee: 0.0,
            status: [0; 32],
            timestamp: 0,
        };
        
        let result = unsafe {
            dinero_wallet_get_swap_status(c_swap_id.as_ptr(), &mut swap)
        };
        
        if result == 0 {
            Ok(swap)
        } else {
            Err(anyhow::anyhow!("Failed to get swap status"))
        }
    }
    
    /// Get available liquidity pools
    pub fn get_liquidity_pools() -> Result<Vec<FFI_LiquidityPool>> {
        let mut pools_ptr: *mut FFI_LiquidityPool = ptr::null_mut();
        let mut count: c_int = 0;
        
        let result = unsafe {
            dinero_wallet_get_liquidity_pools(&mut pools_ptr, &mut count)
        };
        
        if result == 0 && !pools_ptr.is_null() && count > 0 {
            let mut pools = Vec::new();
            
            unsafe {
                for i in 0..count {
                    pools.push(*pools_ptr.add(i as usize));
                }
                
                dinero_wallet_free_pools(pools_ptr, count);
            }
            
            Ok(pools)
        } else {
            Ok(Vec::new())
        }
    }
    
    /// Add liquidity to pool
    pub fn add_liquidity(pool_id: &str, amount: f64) -> Result<String> {
        let c_pool_id = CString::new(pool_id)?;
        let mut txid_ptr: *mut c_char = ptr::null_mut();
        
        let result = unsafe {
            dinero_wallet_add_liquidity(c_pool_id.as_ptr(), amount, &mut txid_ptr)
        };
        
        if result == 0 && !txid_ptr.is_null() {
            let txid = unsafe {
                CStr::from_ptr(txid_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(txid_ptr) };
            Ok(txid)
        } else {
            Err(anyhow::anyhow!("Failed to add liquidity"))
        }
    }
    
    /// Remove liquidity from pool
    pub fn remove_liquidity(pool_id: &str, amount: f64) -> Result<String> {
        let c_pool_id = CString::new(pool_id)?;
        let mut txid_ptr: *mut c_char = ptr::null_mut();
        
        let result = unsafe {
            dinero_wallet_remove_liquidity(c_pool_id.as_ptr(), amount, &mut txid_ptr)
        };
        
        if result == 0 && !txid_ptr.is_null() {
            let txid = unsafe {
                CStr::from_ptr(txid_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(txid_ptr) };
            Ok(txid)
        } else {
            Err(anyhow::anyhow!("Failed to remove liquidity"))
        }
    }
    
    /// Create fiat on-ramp order
    pub fn create_fiat_order(
        amount: f64,
        fiat_currency: &str,
        crypto_symbol: &str,
        payment_method: &str,
    ) -> Result<FFI_FiatOrder> {
        let c_fiat = CString::new(fiat_currency)?;
        let c_crypto = CString::new(crypto_symbol)?;
        let c_method = CString::new(payment_method)?;
        
        let mut order = FFI_FiatOrder {
            order_id: [0; 64],
            payment_method: [0; 32],
            fiat_amount: 0.0,
            fiat_currency: [0; 8],
            crypto_amount: 0.0,
            crypto_symbol: [0; 8],
            exchange_rate: 0.0,
            fee: 0.0,
            status: [0; 32],
            payment_url: [0; 256],
            expires_at: 0,
            created_at: 0,
        };
        
        let result = unsafe {
            dinero_wallet_create_fiat_order(
                amount,
                c_fiat.as_ptr(),
                c_crypto.as_ptr(),
                c_method.as_ptr(),
                &mut order,
            )
        };
        
        if result == 0 {
            Ok(order)
        } else {
            Err(anyhow::anyhow!("Failed to create fiat order"))
        }
    }
    
    /// Get fiat order status
    pub fn get_fiat_order_status(order_id: &str) -> Result<FFI_FiatOrder> {
        let c_order_id = CString::new(order_id)?;
        let mut order = FFI_FiatOrder {
            order_id: [0; 64],
            payment_method: [0; 32],
            fiat_amount: 0.0,
            fiat_currency: [0; 8],
            crypto_amount: 0.0,
            crypto_symbol: [0; 8],
            exchange_rate: 0.0,
            fee: 0.0,
            status: [0; 32],
            payment_url: [0; 256],
            expires_at: 0,
            created_at: 0,
        };
        
        let result = unsafe {
            dinero_wallet_get_fiat_order_status(c_order_id.as_ptr(), &mut order)
        };
        
        if result == 0 {
            Ok(order)
        } else {
            Err(anyhow::anyhow!("Failed to get fiat order status"))
        }
    }
    
    /// Get KYC verification status
    pub fn get_kyc_status() -> Result<FFI_KYCStatus> {
        let mut status = FFI_KYCStatus {
            is_verified: false,
            verification_level: [0; 32],
            provider: [0; 32],
            verified_at: 0,
            expires_at: 0,
            country: [0; 3],
        };
        
        let result = unsafe {
            dinero_wallet_get_kyc_status(&mut status)
        };
        
        if result == 0 {
            Ok(status)
        } else {
            Err(anyhow::anyhow!("Failed to get KYC status"))
        }
    }
    
    /// Start KYC verification
    pub fn start_kyc_verification(level: &str, country: &str) -> Result<String> {
        let c_level = CString::new(level)?;
        let c_country = CString::new(country)?;
        let mut url_ptr: *mut c_char = ptr::null_mut();
        
        let result = unsafe {
            dinero_wallet_start_kyc_verification(
                c_level.as_ptr(),
                c_country.as_ptr(),
                &mut url_ptr,
            )
        };
        
        if result == 0 && !url_ptr.is_null() {
            let url = unsafe {
                CStr::from_ptr(url_ptr).to_string_lossy().into_owned()
            };
            unsafe { dinero_wallet_free_string(url_ptr) };
            Ok(url)
        } else {
            Err(anyhow::anyhow!("Failed to start KYC verification"))
        }
    }
}
