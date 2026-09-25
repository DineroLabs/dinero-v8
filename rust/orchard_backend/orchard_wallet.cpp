#include "orchard_wallet.h"
#include "consensus/coin_type.h"
#include <openssl/crypto.h>
#include <cstddef>
#include <algorithm>
#include <cstring>

namespace dinero::orchard {
namespace {
void Check(int32_t status) { if(status!=DINERO_ORCHARD_OK)throw BackendError(status); }
struct CleanViewingBytes {
    FullViewingKeyBytes& bytes;
    ~CleanViewingBytes(){OPENSSL_cleanse(bytes.data(),bytes.size());}
};
static_assert(consensus::DINERO_COIN_TYPE==1448);
static_assert(sizeof(DineroOrchardAddressText)==100);
static_assert(offsetof(DineroOrchardAddressText,text)==4);
}
void WalletKeys::Deleter::operator()(DineroOrchardWalletKeys* handle)const noexcept {
    if(dinero_orchard_wallet_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletKeys WalletKeys::FromSeed(std::span<const uint8_t> seed,uint32_t account) {
    if(dinero_orchard_wallet_coin_type_v1()!=consensus::DINERO_COIN_TYPE)
        throw BackendError(DINERO_ORCHARD_FORMAT);
    DineroOrchardWalletKeys* handle=nullptr;
    Check(dinero_orchard_wallet_keys_v1(seed.data(),seed.size(),account,&handle));
    return WalletKeys(handle);
}
FullViewingKeyBytes WalletKeys::ExportFullViewingKey()const {
    FullViewingKeyBytes bytes{};Check(dinero_orchard_wallet_fvk_v1(handle_.get(),bytes.data()));return bytes;
}
WalletReceiver WalletKeys::Receiver(WalletScope scope,const DiversifierIndex& index)const {
    auto fvk=ExportFullViewingKey();const CleanViewingBytes clean{fvk};
    return WalletReceiver::FromViewingKey(fvk,scope,index);
}
WalletReceiver WalletReceiver::FromViewingKey(const FullViewingKeyBytes& fvk,
    WalletScope scope,const DiversifierIndex& index) {
    std::array<uint8_t,43> raw{};
    Check(dinero_orchard_wallet_receiver_v1(fvk.data(),uint8_t(scope),index.data(),raw.data()));
    return WalletReceiver(raw);
}
WalletReceiver WalletReceiver::DecodeAddress(std::string_view text,WalletNetwork network) {
    std::array<uint8_t,43> raw{};
    Check(dinero_orchard_address_decode_v1(reinterpret_cast<const uint8_t*>(text.data()),text.size(),uint8_t(network),raw.data()));
    return WalletReceiver(raw);
}
std::string WalletReceiver::EncodeAddress(WalletNetwork network)const {
    DineroOrchardAddressText text{};Check(dinero_orchard_address_encode_v1(raw_.data(),uint8_t(network),&text));
    if(text.length>90)throw BackendError(DINERO_ORCHARD_FORMAT);
    return {reinterpret_cast<const char*>(text.text),text.length};
}

static_assert(sizeof(DineroOrchardPayment)==568);
static_assert(offsetof(DineroOrchardPayment,recipient)==8);
static_assert(offsetof(DineroOrchardPayment,memo)==51);
static_assert(sizeof(DineroOrchardBuiltBundle)==65540);
void WalletShieldPlan::Deleter::operator()(DineroOrchardShieldPlan* handle)const noexcept {
    if(dinero_orchard_shield_plan_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletShieldPlan::WalletShieldPlan(DineroOrchardShieldPlan* handle):handle_(handle) {
    Check(dinero_orchard_shield_facts_v1(handle_.get(),&facts_));
}
WalletShieldPlan WalletShieldPlan::Prepare(const WalletKeys& keys,std::span<const WalletPayment> payments) {
    if(payments.empty()||payments.size()>kMaxActionsV1)throw BackendError(DINERO_ORCHARD_LIMIT);
    std::vector<DineroOrchardPayment> wire(payments.size());
    for(size_t i=0;i<payments.size();++i) {
        wire[i].amount=payments[i].amount_una;
        std::copy(payments[i].recipient.Raw().begin(),payments[i].recipient.Raw().end(),wire[i].recipient);
        std::copy(payments[i].memo.begin(),payments[i].memo.end(),wire[i].memo);
    }
    DineroOrchardShieldPlan* handle=nullptr;
    Check(dinero_orchard_prepare_shield_v1(keys.handle_.get(),wire.data(),wire.size(),&handle));
    return WalletShieldPlan(handle);
}
ProvedWalletBundle WalletShieldPlan::Prove(const SigningContext& context)&& {
    auto consumed=std::move(handle_);
    if(!consumed)throw BackendError(DINERO_ORCHARD_FORMAT);
    const auto digest=context.Digest(facts_);
    auto result=std::make_unique<DineroOrchardBuiltBundle>();
    Check(dinero_orchard_prove_shield_v1(consumed.get(),digest.data(),facts_.effect,
        context.RequiredValueBalance(),result.get()));
    if(result->length>DINERO_ORCHARD_V1_MAX_BUNDLE_BYTES)throw BackendError(DINERO_ORCHARD_FORMAT);
    std::vector<uint8_t> bytes(result->bytes,result->bytes+result->length);
    const auto parsed=ParsedBundle::Decode(bytes);
    auto expected=facts_;
    const auto& actual=parsed.UnverifiedFacts();
    std::copy(std::begin(actual.authorization),std::end(actual.authorization),expected.authorization);
    // ABI has no padding (also asserted by backend.cpp); compare every field,
    // including all unused zero slots, before allowing authorization to escape.
    static_assert(sizeof(DineroOrchardFacts)==624);
    if(std::memcmp(&expected,&actual,sizeof(expected))!=0)throw BackendError(DINERO_ORCHARD_FORMAT);
    return ProvedWalletBundle(std::move(bytes),parsed.VerifyAuthorization(context));
}
} // namespace dinero::orchard
