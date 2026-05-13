use tauri::Manager;
use tauri_plugin_log::LogTarget;

mod wallet;
mod commands;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(
            tauri_plugin_log::Builder::default()
                .targets([
                    LogTarget::LogDir,
                    LogTarget::Stdout,
                    LogTarget::Webview,
                ])
                .build(),
        )
        .plugin(tauri_plugin_camera::init())
        .plugin(tauri_plugin_biometric::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_secure_store::init())
        .invoke_handler(tauri::generate_handler![
            // Wallet initialization
            commands::init_wallet,
            commands::create_wallet,
            commands::restore_wallet,
            // Wallet encryption
            commands::encrypt_wallet,
            commands::unlock_wallet,
            commands::lock_wallet,
            commands::is_wallet_encrypted,
            commands::is_wallet_locked,
            // Balance & addresses
            commands::get_balance,
            commands::get_new_address,
            // Transactions
            commands::send_transaction,
            // Payment UX
            commands::export_history,
            commands::parse_payment_uri,
            commands::generate_payment_uri,
            commands::check_new_transactions,
            // Performance & Diagnostics
            commands::get_sync_progress,
            commands::get_last_error,
            commands::get_error_message,
            // Security
            commands::secure_storage_available,
            commands::store_wallet_secure,
            commands::retrieve_wallet_secure,
            // Transaction Details
            commands::get_tx_confirmations,
            // Exchange & Swap
            commands::get_exchange_rate,
            commands::create_swap_tx,
            commands::get_swap_status,
            // Liquidity & On-Ramp
            commands::get_liquidity_pools,
            commands::add_liquidity,
            commands::remove_liquidity,
            commands::create_fiat_order,
            commands::get_fiat_order_status,
            commands::get_kyc_status,
            commands::start_kyc_verification,
        ])
        .setup(|app| {
            // Initialize wallet on app startup
            let app_data_dir = app.path().app_data_dir().unwrap();
            let wallet_dir = app_data_dir.join("wallet");
            std::fs::create_dir_all(&wallet_dir).unwrap();
            
            // Initialize wallet with FFI
            if let Err(e) = wallet::wallet::init(wallet_dir.to_str().unwrap()) {
                eprintln!("Warning: Failed to initialize wallet: {}", e);
            }
            
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

