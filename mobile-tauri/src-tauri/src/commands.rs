// Tauri command handlers
use crate::wallet::wallet;
use serde::{Deserialize, Serialize};
use tauri_plugin_secure_store::Store;
use tauri::Manager;

#[tauri::command]
pub async fn init_wallet(datadir: String) -> Result<(), String> {
    wallet::init(&datadir).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn create_wallet() -> Result<String, String> {
    wallet::create().map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn restore_wallet(mnemonic: String, passphrase: Option<String>) -> Result<(), String> {
    wallet::restore(&mnemonic, passphrase.as_deref()).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn encrypt_wallet(password: String) -> Result<(), String> {
    wallet::encrypt(&password).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn unlock_wallet(password: String, timeout_seconds: i32) -> Result<(), String> {
    wallet::unlock(&password, timeout_seconds).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn lock_wallet() -> Result<(), String> {
    wallet::lock().map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn is_wallet_encrypted() -> Result<bool, String> {
    Ok(wallet::is_encrypted())
}

#[tauri::command]
pub async fn is_wallet_locked() -> Result<bool, String> {
    Ok(wallet::is_locked())
}

#[tauri::command]
pub async fn get_balance() -> Result<wallet::FFI_WalletBalance, String> {
    wallet::get_balance().map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn get_new_address(label: Option<String>) -> Result<String, String> {
    wallet::get_new_address(label.as_deref()).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn send_transaction(
    to: String,
    amount: f64,
    fee_rate: f64,
    note: Option<String>,
) -> Result<String, String> {
    wallet::send_transaction(&to, amount, fee_rate, note.as_deref())
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn export_history(format: String, dest: String) -> Result<String, String> {
    wallet::export_transactions(&format, &dest)
        .map(|_| format!("✅ Exported transaction history to {}", dest))
        .map_err(|e| format!("❌ Export failed: {}", e))
}

#[tauri::command]
pub async fn parse_payment_uri(uri: String) -> Result<serde_json::Value, String> {
    wallet::parse_payment_uri(&uri)
        .map(|qr| {
            // Convert byte arrays to strings
            let address = String::from_utf8_lossy(&qr.address)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let label = String::from_utf8_lossy(&qr.label)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "address": address,
                "amount": qr.amount,
                "label": label,
            })
        })
        .map_err(|e| format!("❌ Invalid Dinero payment URI: {}", e))
}

#[tauri::command]
pub async fn generate_payment_uri(
    address: String,
    amount: Option<f64>,
    label: Option<String>,
) -> Result<String, String> {
    wallet::generate_payment_uri(&address, amount, label.as_deref())
        .map_err(|e| format!("❌ Failed to generate URI: {}", e))
}

#[tauri::command]
pub async fn check_new_transactions() -> Result<Vec<serde_json::Value>, String> {
    wallet::check_new_transactions()
        .map(|notifs| {
            notifs.into_iter().map(|n| {
                let txid = String::from_utf8_lossy(&n.txid)
                    .trim_matches(char::from(0))
                    .trim()
                    .to_string();
                let address = String::from_utf8_lossy(&n.address)
                    .trim_matches(char::from(0))
                    .trim()
                    .to_string();
                let category = String::from_utf8_lossy(&n.category)
                    .trim_matches(char::from(0))
                    .trim()
                    .to_string();
                
                serde_json::json!({
                    "txid": txid,
                    "address": address,
                    "amount": n.amount,
                    "confirmations": n.confirmations,
                    "timestamp": n.timestamp,
                    "category": category,
                    "is_new": n.is_new,
                })
            }).collect()
        })
        .map_err(|e| format!("❌ Failed to check transactions: {}", e))
}

#[tauri::command]
pub async fn get_sync_progress() -> Result<serde_json::Value, String> {
    wallet::get_sync_progress()
        .map(|progress| {
            let status = if progress.status_message.is_null() {
                String::new()
            } else {
                unsafe {
                    std::ffi::CStr::from_ptr(progress.status_message)
                        .to_string_lossy()
                        .into_owned()
                }
            };
            
            serde_json::json!({
                "progress": progress.progress,
                "current_block": progress.current_block,
                "total_blocks": progress.total_blocks,
                "is_syncing": progress.is_syncing,
                "status_message": status,
            })
        })
        .map_err(|e| format!("❌ Failed to get sync progress: {}", e))
}

#[tauri::command]
pub async fn get_last_error() -> Result<i32, String> {
    Ok(wallet::get_last_error() as i32)
}

#[tauri::command]
pub async fn get_error_message(error_code: i32) -> Result<String, String> {
    let code = match error_code {
        0 => wallet::DineroErrorCode::Success,
        -1 => wallet::DineroErrorCode::ErrorGeneric,
        -2 => wallet::DineroErrorCode::ErrorWalletNotFound,
        -3 => wallet::DineroErrorCode::ErrorWalletLocked,
        -4 => wallet::DineroErrorCode::ErrorWalletEncrypted,
        -5 => wallet::DineroErrorCode::ErrorInvalidMnemonic,
        -6 => wallet::DineroErrorCode::ErrorInvalidAddress,
        -7 => wallet::DineroErrorCode::ErrorInsufficientFunds,
        -8 => wallet::DineroErrorCode::ErrorInvalidAmount,
        -9 => wallet::DineroErrorCode::ErrorTxBroadcastFailed,
        -10 => wallet::DineroErrorCode::ErrorFileIo,
        -11 => wallet::DineroErrorCode::ErrorInvalidFormat,
        -12 => wallet::DineroErrorCode::ErrorNetwork,
        -13 => wallet::DineroErrorCode::ErrorAuthentication,
        -14 => wallet::DineroErrorCode::ErrorNotImplemented,
        _ => wallet::DineroErrorCode::ErrorGeneric,
    };
    
    wallet::get_error_message(code)
        .map_err(|e| format!("❌ Failed to get error message: {}", e))
}

#[tauri::command]
pub async fn secure_storage_available() -> Result<bool, String> {
    Ok(wallet::secure_storage_available())
}

#[tauri::command]
pub async fn store_wallet_secure(
    app: tauri::AppHandle,
    wallet_data: String,
) -> Result<(), String> {
    // Use Tauri's secure-store plugin (uses Keychain on macOS/iOS, Keystore on Android)
    let store = Store::new(&app);
    store
        .set("wallet_data", wallet_data)
        .map_err(|e| format!("Failed to store wallet: {}", e))?;
    Ok(())
}

#[tauri::command]
pub async fn retrieve_wallet_secure(
    app: tauri::AppHandle,
) -> Result<String, String> {
    let store = Store::new(&app);
    store
        .get("wallet_data")
        .map_err(|e| format!("Failed to retrieve wallet: {}", e))
}

#[tauri::command]
pub async fn get_tx_confirmations(txid: String) -> Result<i32, String> {
    wallet::get_tx_confirmations(&txid)
        .map_err(|e| format!("❌ Failed to get confirmations: {}", e))
}

#[tauri::command]
pub async fn get_exchange_rate(
    from: String,
    to: String,
    amount: f64,
) -> Result<serde_json::Value, String> {
    wallet::get_exchange_rate(&from, &to, amount)
        .map(|rate| {
            let from_sym = String::from_utf8_lossy(&rate.from_symbol)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let to_sym = String::from_utf8_lossy(&rate.to_symbol)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let provider = String::from_utf8_lossy(&rate.provider)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "from_symbol": from_sym,
                "to_symbol": to_sym,
                "rate": rate.rate,
                "to_amount": rate.to_amount,
                "min_amount": rate.min_amount,
                "max_amount": rate.max_amount,
                "timestamp": rate.timestamp,
                "provider": provider,
            })
        })
        .map_err(|e| format!("❌ Failed to get exchange rate: {}", e))
}

#[tauri::command]
pub async fn create_swap_tx(
    from_address: String,
    to_address: String,
    amount: f64,
    from_symbol: String,
    to_symbol: String,
) -> Result<serde_json::Value, String> {
    wallet::create_swap_tx(&from_address, &to_address, amount, &from_symbol, &to_symbol)
        .map(|swap| {
            let txid = String::from_utf8_lossy(&swap.txid)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let from_addr = String::from_utf8_lossy(&swap.from_address)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let to_addr = String::from_utf8_lossy(&swap.to_address)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let from_sym = String::from_utf8_lossy(&swap.from_symbol)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let to_sym = String::from_utf8_lossy(&swap.to_symbol)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let status = String::from_utf8_lossy(&swap.status)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "txid": txid,
                "from_address": from_addr,
                "to_address": to_addr,
                "from_amount": swap.from_amount,
                "to_amount": swap.to_amount,
                "from_symbol": from_sym,
                "to_symbol": to_sym,
                "fee": swap.fee,
                "status": status,
                "timestamp": swap.timestamp,
            })
        })
        .map_err(|e| format!("❌ Failed to create swap: {}", e))
}

#[tauri::command]
pub async fn get_swap_status(swap_id: String) -> Result<serde_json::Value, String> {
    wallet::get_swap_status(&swap_id)
        .map(|swap| {
            let txid = String::from_utf8_lossy(&swap.txid)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let status = String::from_utf8_lossy(&swap.status)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "txid": txid,
                "from_amount": swap.from_amount,
                "to_amount": swap.to_amount,
                "status": status,
                "timestamp": swap.timestamp,
            })
        })
        .map_err(|e| format!("❌ Failed to get swap status: {}", e))
}

#[tauri::command]
pub async fn get_liquidity_pools() -> Result<Vec<serde_json::Value>, String> {
    wallet::get_liquidity_pools()
        .map(|pools| {
            pools.into_iter().map(|p| {
                let pool_id = String::from_utf8_lossy(&p.pool_id)
                    .trim_matches(char::from(0))
                    .trim()
                    .to_string();
                let symbol = String::from_utf8_lossy(&p.symbol)
                    .trim_matches(char::from(0))
                    .trim()
                    .to_string();
                
                serde_json::json!({
                    "pool_id": pool_id,
                    "symbol": symbol,
                    "total_liquidity": p.total_liquidity,
                    "available_liquidity": p.available_liquidity,
                    "apy": p.apy,
                    "min_deposit": p.min_deposit,
                    "max_deposit": p.max_deposit,
                    "last_update": p.last_update,
                })
            }).collect()
        })
        .map_err(|e| format!("❌ Failed to get liquidity pools: {}", e))
}

#[tauri::command]
pub async fn add_liquidity(
    pool_id: String,
    amount: f64,
) -> Result<String, String> {
    wallet::add_liquidity(&pool_id, amount)
        .map_err(|e| format!("❌ Failed to add liquidity: {}", e))
}

#[tauri::command]
pub async fn remove_liquidity(
    pool_id: String,
    amount: f64,
) -> Result<String, String> {
    wallet::remove_liquidity(&pool_id, amount)
        .map_err(|e| format!("❌ Failed to remove liquidity: {}", e))
}

#[tauri::command]
pub async fn create_fiat_order(
    amount: f64,
    fiat_currency: String,
    crypto_symbol: String,
    payment_method: String,
) -> Result<serde_json::Value, String> {
    wallet::create_fiat_order(&amount, &fiat_currency, &crypto_symbol, &payment_method)
        .map(|order| {
            let order_id = String::from_utf8_lossy(&order.order_id)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let payment_method = String::from_utf8_lossy(&order.payment_method)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let fiat_currency = String::from_utf8_lossy(&order.fiat_currency)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let crypto_symbol = String::from_utf8_lossy(&order.crypto_symbol)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let status = String::from_utf8_lossy(&order.status)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let payment_url = String::from_utf8_lossy(&order.payment_url)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "order_id": order_id,
                "payment_method": payment_method,
                "fiat_amount": order.fiat_amount,
                "fiat_currency": fiat_currency,
                "crypto_amount": order.crypto_amount,
                "crypto_symbol": crypto_symbol,
                "exchange_rate": order.exchange_rate,
                "fee": order.fee,
                "status": status,
                "payment_url": payment_url,
                "expires_at": order.expires_at,
                "created_at": order.created_at,
            })
        })
        .map_err(|e| format!("❌ Failed to create fiat order: {}", e))
}

#[tauri::command]
pub async fn get_fiat_order_status(order_id: String) -> Result<serde_json::Value, String> {
    wallet::get_fiat_order_status(&order_id)
        .map(|order| {
            let order_id = String::from_utf8_lossy(&order.order_id)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let status = String::from_utf8_lossy(&order.status)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "order_id": order_id,
                "fiat_amount": order.fiat_amount,
                "crypto_amount": order.crypto_amount,
                "status": status,
                "created_at": order.created_at,
            })
        })
        .map_err(|e| format!("❌ Failed to get fiat order status: {}", e))
}

#[tauri::command]
pub async fn get_kyc_status() -> Result<serde_json::Value, String> {
    wallet::get_kyc_status()
        .map(|status| {
            let level = String::from_utf8_lossy(&status.verification_level)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let provider = String::from_utf8_lossy(&status.provider)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            let country = String::from_utf8_lossy(&status.country)
                .trim_matches(char::from(0))
                .trim()
                .to_string();
            
            serde_json::json!({
                "is_verified": status.is_verified,
                "verification_level": level,
                "provider": provider,
                "verified_at": status.verified_at,
                "expires_at": status.expires_at,
                "country": country,
            })
        })
        .map_err(|e| format!("❌ Failed to get KYC status: {}", e))
}

#[tauri::command]
pub async fn start_kyc_verification(
    level: String,
    country: String,
) -> Result<String, String> {
    wallet::start_kyc_verification(&level, &country)
        .map_err(|e| format!("❌ Failed to start KYC verification: {}", e))
}

