#include "wallet/bip86_descriptor.h"
#include "wallet/bip84_descriptor.h"
#include "wallet/descriptor_checksum.h"
#include "wallet/wallet_policy.h"
#include <iostream>

int main() {
    std::cout << "=== BIP86 Descriptor Factory Test ===" << std::endl << std::endl;

    // Test data (same as our actual wallet)
    std::string fingerprint = "4c6d968f";
    std::string xpub = "7XPnQvz8gAqT1FovYBxa3A2YDomymbrg9V72VRaDB2i521VNSqjrp2veQvMPr5G2yrb7fzUvefXMVZ9Y18mNWLs5TkbR8LTyR7n924L6mV45t2nhXamTx";

    // Test BIP86 (Taproot) descriptor generation
    std::cout << "1. BIP86 Taproot Descriptors:" << std::endl;
    auto [receive_tr, change_tr] = din::BIP86DescriptorFactory::createDefaultDescriptors(
        fingerprint, xpub, 1447);

    std::string receive_tr_checksum = din::DescriptorChecksum::AddChecksum(receive_tr);
    std::string change_tr_checksum = din::DescriptorChecksum::AddChecksum(change_tr);

    std::cout << "  Receive: " << receive_tr_checksum << std::endl;
    std::cout << "  Change:  " << change_tr_checksum << std::endl << std::endl;

    // Test BIP84 (SegWit) descriptor generation for comparison
    std::cout << "2. BIP84 SegWit Descriptors:" << std::endl;
    auto [receive_sw, change_sw] = din::BIP84DescriptorFactory::createDefaultDescriptors(
        fingerprint, xpub, 1447);

    std::string receive_sw_checksum = din::DescriptorChecksum::AddChecksum(receive_sw);
    std::string change_sw_checksum = din::DescriptorChecksum::AddChecksum(change_sw);

    std::cout << "  Receive: " << receive_sw_checksum << std::endl;
    std::cout << "  Change:  " << change_sw_checksum << std::endl << std::endl;

    // Test descriptor parsing
    std::cout << "3. Descriptor Parsing:" << std::endl;
    auto parsed_tr = din::BIP86DescriptorFactory::parseDescriptor(receive_tr);
    if (parsed_tr.valid) {
        std::cout << "  ✅ BIP86 parse success" << std::endl;
        std::cout << "     Fingerprint: " << parsed_tr.fingerprint << std::endl;
        std::cout << "     Is change: " << (parsed_tr.is_change ? "yes" : "no") << std::endl;
    } else {
        std::cout << "  ❌ BIP86 parse failed: " << parsed_tr.error << std::endl;
    }

    auto parsed_sw = din::BIP84DescriptorFactory::parseDescriptor(receive_sw);
    if (parsed_sw.valid) {
        std::cout << "  ✅ BIP84 parse success" << std::endl;
        std::cout << "     Fingerprint: " << parsed_sw.fingerprint << std::endl;
        std::cout << "     Is change: " << (parsed_sw.is_change ? "yes" : "no") << std::endl;
    } else {
        std::cout << "  ❌ BIP84 parse failed: " << parsed_sw.error << std::endl;
    }
    std::cout << std::endl;

    // Test WalletPolicy enum
    std::cout << "4. WalletPolicy Enum:" << std::endl;
    auto bip84_str = dinero::WalletPolicyToString(dinero::WalletPolicy::BIP84_LEGACY);
    auto bip86_str = dinero::WalletPolicyToString(dinero::WalletPolicy::BIP86_TAPROOT);

    std::cout << "  BIP84_LEGACY  -> \"" << bip84_str << "\"" << std::endl;
    std::cout << "  BIP86_TAPROOT -> \"" << bip86_str << "\"" << std::endl;

    auto parsed_84 = dinero::StringToWalletPolicy("bip84");
    auto parsed_86 = dinero::StringToWalletPolicy("bip86");

    std::cout << "  \"bip84\" -> " << (parsed_84 == dinero::WalletPolicy::BIP84_LEGACY ? "BIP84_LEGACY ✅" : "ERROR ❌") << std::endl;
    std::cout << "  \"bip86\" -> " << (parsed_86 == dinero::WalletPolicy::BIP86_TAPROOT ? "BIP86_TAPROOT ✅" : "ERROR ❌") << std::endl;
    std::cout << std::endl;

    // Test policy descriptions
    std::cout << "5. Policy Descriptions:" << std::endl;
    std::cout << "  BIP84: " << dinero::WalletPolicyDescription(dinero::WalletPolicy::BIP84_LEGACY) << std::endl;
    std::cout << "  BIP86: " << dinero::WalletPolicyDescription(dinero::WalletPolicy::BIP86_TAPROOT) << std::endl;
    std::cout << std::endl;

    // Verify checksum computation works for both
    std::cout << "6. Checksum Verification:" << std::endl;
    bool tr_valid = din::DescriptorChecksum::Verify(receive_tr_checksum);
    bool sw_valid = din::DescriptorChecksum::Verify(receive_sw_checksum);

    std::cout << "  BIP86 checksum: " << (tr_valid ? "✅ valid" : "❌ invalid") << std::endl;
    std::cout << "  BIP84 checksum: " << (sw_valid ? "✅ valid" : "❌ invalid") << std::endl;
    std::cout << std::endl;

    std::cout << "=== All Tests Complete ===" << std::endl;
    return 0;
}
