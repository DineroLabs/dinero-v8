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

static_assert(sizeof(DineroOrchardNoteFacts)==632);
static_assert(offsetof(DineroOrchardNoteFacts,memo)==115);
static_assert(sizeof(DineroOrchardWitnessFacts)==1104);
static_assert(offsetof(DineroOrchardWitnessFacts,path)==80);
static_assert(sizeof(DineroOrchardSpendInput)==1040);
static_assert(offsetof(DineroOrchardSpendInput,note)==1032);
void WalletNote::Deleter::operator()(DineroOrchardNote* handle)const noexcept {
    if(dinero_orchard_note_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletNote::WalletNote(DineroOrchardNote* handle):handle_(handle) {Check(dinero_orchard_note_facts_v1(handle,&facts_));}
WalletNote::~WalletNote(){OPENSSL_cleanse(&facts_,sizeof(facts_));}
std::optional<WalletNote> WalletNote::Receive(const VerifiedAuthorization& authorization,
    const FullViewingKeyBytes& fvk,WalletScope scope,uint32_t index) {
    DineroOrchardNote* handle=nullptr;
    Check(dinero_orchard_receive_note_v1(authorization.handle_.get(),fvk.data(),uint8_t(scope),index,&handle));
    if(!handle)return std::nullopt;
    return WalletNote(handle);
}
void WalletWitness::Deleter::operator()(DineroOrchardWitness* handle)const noexcept {
    if(dinero_orchard_witness_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletWitness::WalletWitness(DineroOrchardWitness* handle):handle_(handle) {Check(dinero_orchard_witness_facts_v1(handle,&facts_));}
WalletWitness WalletWitness::ForAppendedLeaf(const OrchardFrontier& parent,std::span<const Hash> commitments,size_t index) {
    DineroOrchardWitness* handle=nullptr;
    Check(dinero_orchard_witness_create_v1(parent.Bytes().data(),parent.Bytes().size(),
        commitments.empty()?nullptr:commitments[0].data(),commitments.size(),index,&handle));
    return WalletWitness(handle);
}
WalletWitness WalletWitness::Append(std::span<const Hash> commitments,const Hash& parent,const Hash& next)const {
    DineroOrchardWitness* handle=nullptr;
    Check(dinero_orchard_witness_append_v1(handle_.get(),commitments.empty()?nullptr:commitments[0].data(),
        commitments.size(),parent.data(),next.data(),&handle));return WalletWitness(handle);
}
static_assert(sizeof(DineroOrchardStoredWitness)==4100);
std::vector<uint8_t> WalletWitness::Encode()const {
    DineroOrchardStoredWitness result{};Check(dinero_orchard_witness_encode_v1(handle_.get(),&result));
    if(result.length>DINERO_ORCHARD_V1_MAX_WITNESS_BYTES)throw BackendError(DINERO_ORCHARD_FORMAT);
    return {result.bytes,result.bytes+result.length};
}
WalletWitness WalletWitness::Decode(std::span<const uint8_t> bytes,const Hash& commitment,const Hash& root,uint64_t count) {
    DineroOrchardWitness* handle=nullptr;
    Check(dinero_orchard_witness_decode_v1(bytes.data(),bytes.size(),commitment.data(),root.data(),count,&handle));
    return WalletWitness(handle);
}
namespace {
std::vector<DineroOrchardPayment> Payments(std::span<const WalletPayment> payments) {
    if(payments.size()>kMaxActionsV1)throw BackendError(DINERO_ORCHARD_LIMIT);
    std::vector<DineroOrchardPayment> wire(payments.size());
    for(size_t i=0;i<payments.size();++i) {
        wire[i].amount=payments[i].amount_una;
        std::copy(payments[i].recipient.Raw().begin(),payments[i].recipient.Raw().end(),wire[i].recipient);
        std::copy(payments[i].memo.begin(),payments[i].memo.end(),wire[i].memo);
    }
    return wire;
}
}
static_assert(sizeof(DineroOrchardPayment)==568);
static_assert(offsetof(DineroOrchardPayment,recipient)==8);
static_assert(offsetof(DineroOrchardPayment,memo)==51);
static_assert(sizeof(DineroOrchardBuiltBundle)==65540);
void WalletBundlePlan::Deleter::operator()(DineroOrchardWalletPlan* handle)const noexcept {
    if(dinero_orchard_wallet_plan_free_v1(handle)!=DINERO_ORCHARD_OK)std::terminate();
}
WalletBundlePlan::WalletBundlePlan(DineroOrchardWalletPlan* handle):handle_(handle) {
    Check(dinero_orchard_wallet_plan_facts_v1(handle_.get(),&facts_));
}
WalletBundlePlan WalletBundlePlan::PrepareShield(const WalletKeys& keys,std::span<const WalletPayment> payments) {
    if(payments.empty()||payments.size()>kMaxActionsV1)throw BackendError(DINERO_ORCHARD_LIMIT);
    const auto wire=Payments(payments);
    DineroOrchardWalletPlan* handle=nullptr;
    Check(dinero_orchard_prepare_shield_v1(keys.handle_.get(),wire.data(),wire.size(),&handle));
    return WalletBundlePlan(handle);
}
WalletBundlePlan WalletBundlePlan::PrepareSpend(const WalletKeys& keys,std::span<const WalletSpendInput> inputs,
    const Hash& anchor,std::span<const WalletPayment> payments) {
    if(inputs.empty()||inputs.size()>kMaxActionsV1)throw BackendError(DINERO_ORCHARD_LIMIT);
    const auto outputs=Payments(payments);std::vector<DineroOrchardSpendInput> wire(inputs.size());
    for(size_t i=0;i<inputs.size();++i) {
        const auto& witness=inputs[i].witness.Facts();const auto& note=inputs[i].note;
        if(!std::equal(anchor.begin(),anchor.end(),witness.root)||
           !std::equal(std::begin(note.Facts().commitment),std::end(note.Facts().commitment),witness.commitment))
            throw BackendError(DINERO_ORCHARD_FORMAT);
        wire[i].position=witness.position;
        std::memcpy(wire[i].path,witness.path,sizeof(witness.path));wire[i].note=note.handle_.get();
    }
    DineroOrchardWalletPlan* handle=nullptr;
    Check(dinero_orchard_prepare_spend_v1(keys.handle_.get(),wire.data(),wire.size(),anchor.data(),
        outputs.data(),outputs.size(),&handle));return WalletBundlePlan(handle);
}
ProvedWalletBundle WalletBundlePlan::Prove(const SigningContext& context)&& {
    auto consumed=std::move(handle_);
    if(!consumed)throw BackendError(DINERO_ORCHARD_FORMAT);
    const auto digest=context.Digest(facts_);
    auto result=std::make_unique<DineroOrchardBuiltBundle>();
    Check(dinero_orchard_prove_wallet_bundle_v1(consumed.get(),digest.data(),facts_.effect,
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
