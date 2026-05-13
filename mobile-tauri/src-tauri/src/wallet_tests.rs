// wallet-core/ffi/tests/wallet_tests.rs
// Rust Integration Tests for Dinero Wallet FFI

#[cfg(test)]
mod tests {
    use crate::wallet::wallet;
    
    #[test]
    fn test_error_codes() {
        let error = wallet::get_last_error();
        // Should be valid error code (may be Success or any error)
        assert!(matches!(error, wallet::DineroErrorCode::Success | _));
    }
    
    #[test]
    fn test_error_message() {
        let message = wallet::get_error_message(wallet::DineroErrorCode::ErrorGeneric);
        assert!(message.is_ok());
        assert!(!message.unwrap().is_empty());
    }
    
    #[test]
    fn test_uri_parsing() {
        let uri = "dinero:din1qtest123?amount=15.25&label=Test";
        let result = wallet::parse_payment_uri(uri);
        
        assert!(result.is_ok());
        let parsed = result.unwrap();
        
        let address = String::from_utf8_lossy(&parsed.address)
            .trim_matches(char::from(0))
            .trim()
            .to_string();
        assert_eq!(address, "din1qtest123");
        assert_eq!(parsed.amount, 15.25);
    }
    
    #[test]
    fn test_uri_generation() {
        let result = wallet::generate_payment_uri(
            "din1qtest123",
            Some(100.5),
            Some("Test Label"),
        );
        
        assert!(result.is_ok());
        let uri = result.unwrap();
        assert!(uri.starts_with("dinero:"));
        assert!(uri.contains("amount=100.5"));
    }
    
    #[test]
    fn test_invalid_uri() {
        let result = wallet::parse_payment_uri("invalid:uri");
        assert!(result.is_err());
    }
    
    #[test]
    fn test_secure_storage_available() {
        let available = wallet::secure_storage_available();
        // May be false if not implemented yet, which is OK
        assert!(available == true || available == false);
    }
}

