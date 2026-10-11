// The two escrow builders must never produce an output a single party can
// take, or one the chain cannot spend at all.
//
// - BuildEscrow used the buyer's key as the Taproot internal key, so the buyer
//   could key-path spend the escrow alone and skip both script leaves.
// - EscrowContractBuilder emitted a P2SH escrow with a placeholder address
//   ("din1q" + hash). Dinero consensus spends only P2PKH, P2WPKH, P2TR and
//   P2MR, so funds sent there could never be released or refunded.
#include "contracts/escrow_contract.h"
#include "wallet/taproot_template_builder.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace {
// BIP341's NUMS point H: no known discrete log, so no key-path spend exists.
constexpr std::array<uint8_t, 32> kNums{
    0x50, 0x92, 0x9b, 0x74, 0xc1, 0xa0, 0x49, 0x54, 0xb7, 0x8b, 0x4b, 0x60, 0x35, 0xe9, 0x7a, 0x5e,
    0x07, 0x8a, 0x5a, 0x0f, 0x28, 0xec, 0x96, 0xd5, 0x47, 0xbf, 0xee, 0x9a, 0xce, 0x80, 0x3a, 0xc0};
// x-coordinate of the secp256k1 generator: a valid x-only key.
const std::vector<uint8_t> kBuyer{
    0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
    0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98};
}  // namespace

TEST(EscrowBuildersFailSafe, TaprootEscrowHasNoBuyerKeyPath) {
    dinero::EscrowTemplateParams params;
    params.buyer_pubkey = kBuyer;
    params.seller_pubkey = std::vector<uint8_t>(32, 0x22);
    params.attestor_pubkeys = {std::vector<uint8_t>(32, 0x33)};
    params.attestor_threshold = 2;
    params.timeout_blocks = 144;

    const auto result = dinero::TaprootTemplateBuilder::BuildEscrow(params);
    ASSERT_EQ(result.leaves.size(), 2U);
    for (const auto& leaf : result.leaves) {
        EXPECT_EQ(leaf.control_block.internal_key, kNums)
            << leaf.label << " leaf: the internal key must be the NUMS point, not the buyer";
    }
}

TEST(EscrowBuildersFailSafe, P2shEscrowIsRefused) {
    using namespace dinero::contracts;
    EscrowKeys keys;
    keys.buyer_pubkey = std::string(66, '2');
    keys.seller_pubkey = std::string(66, '3');
    EXPECT_THROW(EscrowContractBuilder::buildContract(keys, 1.0, 144, EscrowType::TwoOfTwo, 0, 100),
                 std::runtime_error)
        << "a P2SH escrow cannot be spent on Dinero; building one must fail";
}
