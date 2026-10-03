//! PUBLIC TEST KEYS from make_combined_fixture, never a real wallet.
use orchard::keys::{FullViewingKey, SpendingKey};
fn main() {
    let dir = std::path::PathBuf::from(std::env::args().nth(1).expect("fixture directory"));
    for (name, byte) in [("combined-sender.fvk", 7), ("combined-recipient.fvk", 9)] {
        let sk = SpendingKey::from_bytes([byte; 32]).unwrap();
        std::fs::write(dir.join(name), FullViewingKey::from(&sk).to_bytes()).unwrap();
    }
}
